#include "tests/unit/node_fixture.hpp"

#include "node/convert.hpp"
#include "sched/placer.hpp"
#include "sched/rank.hpp"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using zeromesh::sched::Candidate;

namespace v1 = zeromesh::v1;
using zeromesh::testing::AttemptLog;
using zeromesh::testing::ShBatch;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;

namespace {

void Expect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

std::shared_ptr<zeromesh::sched::Placer> PlacerFor(TestNodeFull& self) {
    auto factory = [&self](const zeromesh::net::Peer& p) {
        return std::make_shared<zeromesh::node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port), self.id,
                                                            self.pins, p.id);
    };
    return std::make_shared<zeromesh::sched::Placer>(self.executor, self.peers, factory, self.cas, self.merkle,
                                                     self.node_id, [&self] { return self.telemetry; });
}

} // namespace

TEST(PlacerRunsABatchOnAPeerAndPullsEachItemsOutputs) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);

    TempDir src;
    zeromesh::testing::WriteFile(src / "in.txt", "payload\n");
    auto root = zeromesh::node::ToProto(a.merkle->BuildTree(src.path()));
    auto spec = ShBatch({{"one", "mkdir -p out/sub && cp in.txt out/sub/copy && printf %s \"$ZM_NODE_ID\" > who && echo ran"},
                         {"two", "echo second > two.txt"}},
                        root);
    spec.set_stream_logs(true);
    spec.mutable_items(0)->add_outputs("who");
    spec.mutable_items(0)->add_outputs("out");
    spec.mutable_items(1)->add_outputs("two.txt");
    placer->Execute("run/b1", spec, {}, log.Callbacks());
    ASSERT_TRUE(log.Wait());
    Expect(log.result.failure == v1::FAILURE_NONE, log.result.message);
    ASSERT_EQ(log.result.node, std::string("node-b"));
    ASSERT_TRUE(log.result.bytes_pushed > 0);
    ASSERT_EQ(log.placed.size(), 1u);
    ASSERT_EQ(log.started.size(), 2u);
    ASSERT_TRUE(log.logs.find("ran\n") != std::string::npos);
    ASSERT_EQ(log.items.size(), 2u);

    // Each item's outputs, whole output trees and logs are in A's store.
    for (const auto& [o, node] : log.items) {
        ASSERT_EQ(node, std::string("node-b"));
        Expect(o.failure() == v1::FAILURE_NONE, o.message());
        if (o.id() == "two") {
            ASSERT_EQ(a.cas->GetBlob(o.outputs(0).digest().hash()), std::optional<std::string>("second\n"));
            continue;
        }
        ASSERT_EQ(a.cas->GetBlob(o.stdout().hash()), std::optional<std::string>("ran\n"));
        for (const auto& f : o.outputs()) {
            if (f.path() == "who") {
                ASSERT_EQ(a.cas->GetBlob(f.digest().hash()), std::optional<std::string>("node-b"));
            } else {
                ASSERT_TRUE(f.is_dir());
                TempDir dest;
                ASSERT_TRUE(a.merkle->MaterializeTree(zeromesh::node::FromProto(f.digest()), dest / "out"));
                ASSERT_EQ(zeromesh::testing::ReadFile(dest / "out/sub/copy"), std::string("payload\n"));
            }
        }
    }
}

TEST(PlacerReoffersPrunedInputsAndResendsBlobsAfterGc) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    auto placer = PlacerFor(a);
    TempDir src;
    zeromesh::testing::WriteFile(src / "in.txt", "input survives");
    auto root = a.merkle->BuildTree(src.path());
    auto spec = ShBatch({{"one", "cat in.txt > out.txt"}}, zeromesh::node::ToProto(root));
    spec.mutable_items(0)->add_outputs("out.txt");
    auto run = [&](const std::string& id, bool expect_push) {
        AttemptLog log;
        placer->Execute(id, spec, {}, log.Callbacks());
        ASSERT_TRUE(log.Wait());
        Expect(log.result.failure == v1::FAILURE_NONE, log.result.message);
        ASSERT_EQ(log.items.size(), 1u);
        Expect(log.items[0].first.failure() == v1::FAILURE_NONE, log.items[0].first.message());
        ASSERT_EQ(log.result.bytes_pushed > 0, expect_push);
        ASSERT_EQ(a.cas->GetBlob(log.items[0].first.outputs(0).digest().hash()),
                  std::optional<std::string>("input survives"));
    };
    run("first", true);
    b.cas->CollectGarbage(0, {});
    run("cached", false); // only hard links remain: no input blobs need to be pushed
    for (int i = 0; i < 100; ++i) {
        b.runner->PruneInputs(std::chrono::seconds(0));
        if (!b.runner->CachedInput(root)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    b.cas->CollectGarbage(0, {});
    run("pruned", true); // stale hint gets a full re-offer; old sent timestamps cannot suppress it
}

TEST(PlacerReportsWhyNoNodeCanRunABatch) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    AttemptLog log;
    auto placer = PlacerFor(a);
    {
        AttemptLog log;
        placer->Execute("run/b1", ShBatch({{"x", "true"}}), {}, log.Callbacks());
        ASSERT_TRUE(log.Wait());
        ASSERT_TRUE(log.result.failure == v1::FAILURE_NO_CAPACITY); // nowhere to run, nothing missing
        Expect(log.result.message == "no node can run this batch: no node is online that may run it", log.result.message);
    }
    // A peer without the program: its reason is reported, and the kind says "software".
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    auto spec = ShBatch({{"x", "true"}});
    spec.mutable_env()->add_programs()->set_name("definitely-not-a-binary-xyz");
    placer->Execute("run/b2", spec, {}, log.Callbacks());
    ASSERT_TRUE(log.Wait());
    ASSERT_TRUE(log.result.failure == v1::FAILURE_ENV_UNAVAILABLE);
    Expect(log.result.message.find("no node can run this batch: node-b: ") == 0 &&
               log.result.message.find("is not installed") != std::string::npos,
           log.result.message);
    ASSERT_EQ(log.rejects.size(), 1u);
    ASSERT_TRUE(std::get<1>(log.rejects[0]) == v1::REJECT_ENV);
}

TEST(PlacerReportsAToolchainMismatchAsItsOwnKind) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);
    auto spec = ShBatch({{"x", "true"}});
    auto* prog = spec.mutable_env()->add_programs();
    prog->set_name("sh");
    prog->set_digest(std::string(64, 'f')); // "the requester's sh" is another build
    placer->Execute("run/b1", spec, {}, log.Callbacks());
    ASSERT_TRUE(log.Wait());
    Expect(log.result.failure == v1::FAILURE_TOOLCHAIN_MISMATCH, log.result.message);
}

TEST(PlacerReportsALostDonorFast) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);
    placer->Execute("run/b1", ShBatch({{"x", "sleep 60"}}), {}, log.Callbacks());
    ASSERT_TRUE(log.WaitStarted());
    const auto t0 = std::chrono::steady_clock::now();
    b.Stop();
    ASSERT_TRUE(log.Wait(std::chrono::seconds(15)));
    ASSERT_TRUE(log.result.failure == v1::FAILURE_DONOR_LEFT);
    Expect(log.result.message.find("node node-b lost during the run") == 0, log.result.message);
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15));
    ASSERT_TRUE(log.items.empty());
}

TEST(PlacerReportsADrainedDonorAsLeft) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);
    // The first item finishes before the drain and is kept; the second is stopped by it.
    placer->Execute("run/b1", ShBatch({{"quick", "echo quick"}, {"slow", "sleep 30"}}), {}, log.Callbacks());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard lock(log.mu);
            if (log.started.size() == 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    a.peers->MarkDraining("node-b", "battery");
    ASSERT_TRUE(log.Wait(std::chrono::seconds(20)));
    Expect(log.result.failure == v1::FAILURE_DONOR_LEFT, log.result.message);
    Expect(log.result.message.find("drained") != std::string::npos, log.result.message);
    ASSERT_EQ(log.items.size(), 1u);
    ASSERT_EQ(log.items[0].first.id(), std::string("quick"));
}

TEST(PlacerItemFailuresComeFromTheDonor) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);
    auto spec = ShBatch({{"slow", "sleep 30"}, {"bad", "exit 4"}});
    spec.set_timeout_s(1);
    placer->Execute("run/b1", spec, {}, log.Callbacks());
    ASSERT_TRUE(log.Wait(std::chrono::seconds(30)));
    Expect(log.result.failure == v1::FAILURE_NONE, log.result.message); // the batch itself ran
    ASSERT_EQ(log.items.size(), 2u);
    for (const auto& [o, node] : log.items) {
        if (o.id() == "slow") ASSERT_TRUE(o.failure() == v1::FAILURE_TIMEOUT);
        if (o.id() == "bad") {
            ASSERT_TRUE(o.failure() == v1::FAILURE_EXIT);
            ASSERT_EQ(o.exit_code(), 4);
        }
    }
}

TEST(PlacerCancelEndsTheAttempt) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);
    placer->Execute("run/b1", ShBatch({{"x", "sleep 30"}}), {}, log.Callbacks());
    ASSERT_TRUE(log.WaitStarted());
    placer->Cancel("run/b1");
    a.peers->MarkDraining("node-b", "battery"); // a racing drain changes nothing
    ASSERT_TRUE(log.Wait(std::chrono::seconds(15)));
    ASSERT_TRUE(log.result.cancelled);
    ASSERT_TRUE(log.result.failure == v1::FAILURE_CANCELLED);
}

TEST(PlacerExcludedNodesAreNotUsed) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    auto pb = b.AsPeer();
    pb.cores = 32; // the better-looking donor
    a.peers->Upsert(pb);
    a.peers->Upsert(c.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);
    placer->Execute("run/b1", ShBatch({{"x", "true"}}), {"node-b"}, log.Callbacks());
    ASSERT_TRUE(log.Wait());
    ASSERT_EQ(log.result.node, std::string("node-c"));

    // Exclusion is a preference: when only excluded nodes could run it, they get it again.
    AttemptLog again;
    placer->Execute("run/b2", ShBatch({{"x", "true"}}), {"node-b", "node-c"}, again.Callbacks());
    ASSERT_TRUE(again.Wait());
    Expect(again.result.failure == v1::FAILURE_NONE, again.result.message);
}

TEST(PlacerSaysTheExcludedNodesLeftWhenNoneIsBack) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key);
    a.Start();
    AttemptLog log;
    auto placer = PlacerFor(a);
    placer->Execute("run/b1", ShBatch({{"x", "true"}}), {"node-gone"}, log.Callbacks());
    ASSERT_TRUE(log.Wait());
    // Not "software unavailable": the only node that could run it is gone.
    ASSERT_TRUE(log.result.failure == v1::FAILURE_DONOR_LEFT);
    Expect(log.result.message.find("node-gon: lost earlier in this run") != std::string::npos, log.result.message);
}

TEST(PlacerInFlightCapSpread) {
    // Rank alone: with 2 work cores and 1-core batches, a node whose 2 cores are held is filtered out.
    zeromesh::sched::Need need;
    Candidate a;
    a.node_id = "a";
    a.work_cores = 2;
    a.mem_available = 1ull << 30;
    a.arch = "x86_64";
    a.on_ac = true;
    Candidate b = a;
    b.node_id = "b";
    b.cores_in_use = 2;
    auto ranked = zeromesh::sched::Rank(need, {a, b});
    ASSERT_EQ(ranked.size(), 1u);
    ASSERT_EQ(ranked[0].node_id, std::string("a"));

    // Atomic reservation: two concurrent batches against one peer with 1 core never both run.
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull requester("node-a", key), donor("node-b", key);
    requester.Start();
    donor.Start();
    requester.Admit(donor);
    auto peer = donor.AsPeer();
    peer.work_cores = 1; // what it announces it lets work hold
    peer.cores_free = 0; // a load spike must not change that
    requester.peers->Upsert(peer);
    AttemptLog one, two;
    auto placer = PlacerFor(requester);
    placer->Execute("run/b1", ShBatch({{"x", "sleep 2"}}), {}, one.Callbacks());
    placer->Execute("run/b2", ShBatch({{"y", "sleep 2"}}), {}, two.Callbacks());
    int peak = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        peak = std::max(peak, placer->InFlightOn("node-b"));
        {
            std::lock_guard l1(one.mu);
            std::lock_guard l2(two.mu);
            if (one.done && two.done) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(one.Wait() && two.Wait());
    ASSERT_TRUE(one.result.failure == v1::FAILURE_NONE && two.result.failure == v1::FAILURE_NONE);
    ASSERT_EQ(peak, 1);
}

TEST(PlacerRoutesAroundItsOwnNodeWhenOtherRequestersFillIt) {
    // This node's executor also runs other requesters' batches. While they hold its slots, our
    // batch goes to an idle peer instead of queueing behind them here.
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    ASSERT_TRUE(a.executor->Hold("node-x/b1", ShBatch({{"x", "true"}})));
    ASSERT_TRUE(a.executor->Hold("node-x/b2", ShBatch({{"y", "true"}})));
    AttemptLog log;
    auto placer = PlacerFor(a);
    placer->Execute("run/b1", ShBatch({{"z", "echo ran"}}, {}, v1::ANYWHERE), {}, log.Callbacks());
    ASSERT_TRUE(log.Wait());
    Expect(log.result.failure == v1::FAILURE_NONE, log.result.message);
    ASSERT_EQ(log.result.node, std::string("node-b"));

    // Room again here: this node, holding the input already, runs the next one itself.
    a.executor->Release("node-x/b1");
    a.executor->Release("node-x/b2");
    AttemptLog local;
    placer->Execute("run/b2", ShBatch({{"w", "echo ran"}}, {}, v1::ANYWHERE), {}, local.Callbacks());
    ASSERT_TRUE(local.Wait());
    ASSERT_EQ(local.result.node, std::string("node-a"));
}

TEST(PlacerRoutesAroundADonorThatRefusesOnPolicy) {
    // The donor ranked first is on a low battery and refuses by policy. That is its answer, not
    // the batch's: the idle donor behind it must get the batch.
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), weak("node-weak", key), ok("node-ok", key);
    weak.telemetry.on_ac = false;
    weak.telemetry.battery_pct = 30;
    a.Start();
    weak.Start();
    ok.Start();
    a.Admit(weak);
    a.Admit(ok);
    auto weak_peer = weak.AsPeer();
    weak_peer.work_cores = 16; // looks like the better donor, so it is offered first
    a.peers->Upsert(weak_peer);
    a.peers->Upsert(ok.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);
    placer->Execute("run/b1", ShBatch({{"x", "echo ran"}}), {}, log.Callbacks());
    ASSERT_TRUE(log.Wait());
    Expect(log.result.failure == v1::FAILURE_NONE, log.result.message);
    ASSERT_EQ(log.result.node, std::string("node-ok"));
}

TEST(PlacerFailsABatchLargerThanEveryDonorsLimit) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), small("node-small", key);
    small.policy.max_cores = 2;
    a.Start();
    small.Start();
    a.Admit(small);
    a.peers->Upsert(small.AsPeer());
    AttemptLog log;
    auto placer = PlacerFor(a);
    auto spec = ShBatch({{"x", "true"}});
    spec.mutable_resources()->set_cores(4);
    placer->Execute("run/b1", spec, {}, log.Callbacks());
    ASSERT_TRUE(log.Wait(std::chrono::seconds(10)));
    Expect(log.result.message.find("no node can run this batch: node-sma: lets work hold 2 cores, the batch needs 4") ==
               0,
           log.result.message);
    ASSERT_TRUE(log.result.failure == v1::FAILURE_NO_CAPACITY); // the node's size, not missing software
}

TEST(PlacerHoldsReservationsToThePeersAnnouncedBudget) {
    // A peer with 8 GiB of RAM that lets work use 1 GiB: a batch known to need 2 GiB fits its RAM but
    // never its budget, so it fails at once with why, instead of waiting for memory forever.
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    auto pb = b.AsPeer();
    pb.memory_bytes = 8ull << 30;
    pb.mem_available = 8ull << 30;
    pb.work_memory_bytes = 1ull << 30;
    a.peers->Upsert(pb);
    AttemptLog log;
    auto placer = PlacerFor(a);
    auto spec = ShBatch({{"x", "true"}});
    spec.set_where(v1::REMOTE_ONLY);
    spec.set_mem_need(2ll << 30);
    spec.mutable_resources()->set_mem_bytes(2560ll << 20);
    placer->Execute("run/b1", spec, {}, log.Callbacks());
    ASSERT_TRUE(log.Wait(std::chrono::seconds(10)));
    ASSERT_TRUE(log.result.failure == v1::FAILURE_NO_CAPACITY);
    Expect(log.result.message.find("node-b: lets work use 1024 MiB of memory, the batch needs 2048 MiB") !=
               std::string::npos,
           log.result.message);
}

TEST(PlacerCountsSlotsForTheRunEngine) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    auto pb = b.AsPeer();
    pb.work_cores = 8;
    a.peers->Upsert(pb);
    auto placer = PlacerFor(a);
    v1::Resources one;
    one.set_cores(1);
    // Local: the executor's 2 cores. Peer: the 8 it announces. Nothing runs: all are free.
    auto slots = [&](const v1::Resources& r, v1::Where w) {
        const auto s = placer->SlotsFor(r, w);
        return std::pair{s.total, s.free};
    };
    ASSERT_TRUE(slots(one, v1::ANYWHERE) == std::pair(10, 10));
    ASSERT_TRUE(slots(one, v1::LOCAL_ONLY) == std::pair(2, 2));
    ASSERT_TRUE(slots(one, v1::REMOTE_ONLY) == std::pair(8, 8));
    v1::Resources four;
    four.set_cores(4);
    ASSERT_TRUE(slots(four, v1::REMOTE_ONLY) == std::pair(2, 2));
    // No GPU donor: one slot all the same, so the batch goes to the placer and fails with why.
    v1::Resources gpu;
    gpu.set_gpus(1);
    ASSERT_TRUE(slots(gpu, v1::REMOTE_ONLY) == std::pair(1, 1));

    // What this node's executor holds for anyone is taken; so is what our batches hold on a peer,
    // and our batches still looking for a node.
    ASSERT_TRUE(a.executor->Hold("other-requester/b9", ShBatch({{"x", "true"}})));
    ASSERT_TRUE(slots(one, v1::LOCAL_ONLY) == std::pair(2, 1));
    a.executor->Release("other-requester/b9");
    ASSERT_TRUE(slots(one, v1::LOCAL_ONLY) == std::pair(2, 2));
}

TEST(PlacerWaitsForADonorThatIsBusyNowButBigEnough) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    auto busy = b.AsPeer();
    busy.mem_available = 100ull << 20; // its other work holds the memory this second
    a.peers->Upsert(busy);
    auto spec = ShBatch({{"x", "true"}});
    spec.mutable_resources()->set_mem_bytes(1ll << 30);
    AttemptLog log;
    auto placer = PlacerFor(a);
    placer->Execute("run/b1", spec, {}, log.Callbacks());
    ASSERT_FALSE(log.Wait(std::chrono::milliseconds(800))); // waits: 8 GiB total is enough, given time
    a.peers->Upsert(b.AsPeer());                             // the memory is free again
    ASSERT_TRUE(log.Wait());
    Expect(log.result.failure == v1::FAILURE_NONE, log.result.message);
}

TEST(PlacerSaysWhenNoDonorHasEnoughMemoryAtAll) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer()); // 8 GiB
    auto spec = ShBatch({{"x", "true"}});
    spec.set_mem_need(16ll << 30); // known to need it: no node can take it
    spec.mutable_resources()->set_mem_bytes(20ll << 30);
    AttemptLog log;
    auto placer = PlacerFor(a);
    placer->Execute("run/b1", spec, {}, log.Callbacks());
    ASSERT_TRUE(log.Wait());
    ASSERT_TRUE(log.result.failure == v1::FAILURE_NO_CAPACITY);
    Expect(log.result.message.find("node-b: lets work use 8192 MiB of memory, the batch needs 16384 MiB") !=
               std::string::npos,
           log.result.message);
}

TEST(ExecutorHoldsTheWholeBudgetForALargeReservationAndRefusesALargerNeed) {
    // Reserving more than all work here may use is an estimate: the batch runs alone. Needing more
    // never fits: it fails at once as "no capacity", holding nothing, and the batch behind it runs.
    struct Restore {
        ~Restore() { zeromesh::exec::cgroup::SetUpWork(0); }
    } restore;
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key);
    auto runner = std::make_shared<zeromesh::exec::BatchRunner>(a.cas, a.merkle, a.dir / "scratch-budget",
                                                                 zeromesh::exec::cgroup::SetUpWork(64ull << 20));
    if (!runner->unavailable().empty()) return; // no sandbox or containment here
    zeromesh::node::LocalExecutor executor(runner, a.registry, "node-a", 2, a.dir / "state-budget", a.cas, {});
    ASSERT_EQ(executor.MemoryBudget(), 64ll << 20);

    auto large = ShBatch({{"large", "sleep 0.5"}});
    large.mutable_resources()->set_mem_bytes(128ll << 20);
    auto huge = ShBatch({{"huge", "true"}});
    huge.set_mem_need(128ll << 20);
    auto small = ShBatch({{"small", "true"}});
    small.mutable_resources()->set_mem_bytes(1ll << 20);
    AttemptLog large_log, huge_log, small_log;
    executor.Execute("run/large", large, {}, large_log.Callbacks());
    ASSERT_EQ(executor.MemReserved(), 64ll << 20); // all of the budget, not more
    executor.Execute("run/huge", huge, {}, huge_log.Callbacks());
    executor.Execute("run/small", small, {}, small_log.Callbacks());
    ASSERT_TRUE(large_log.Wait());
    Expect(large_log.result.failure == v1::FAILURE_NONE, large_log.result.message);
    ASSERT_TRUE(huge_log.Wait());
    ASSERT_TRUE(huge_log.result.failure == v1::FAILURE_NO_CAPACITY);
    Expect(huge_log.result.message.find("needs 128 MiB of memory; work on this node may use 64 MiB") != std::string::npos,
           huge_log.result.message);
    ASSERT_TRUE(small_log.Wait());
    Expect(small_log.result.failure == v1::FAILURE_NONE, small_log.result.message);
    ASSERT_EQ(executor.MemReserved(), 0);
}
