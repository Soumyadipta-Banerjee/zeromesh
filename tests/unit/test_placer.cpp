#include "tests/unit/node_fixture.hpp"

#include "node/action_cache.hpp"
#include "node/convert.hpp"
#include "node/job_manager.hpp"
#include "sched/dispatch_policy.hpp"
#include "sched/placer.hpp"
#include "sched/rank.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using zeromesh::sched::Candidate;

namespace v1 = zeromesh::v1;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;

namespace {

void Expect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

// Node `self` with a JobManager whose executor is a Placer over self's PeerTable.
struct Requester {
    std::shared_ptr<zeromesh::sched::Placer> placer;
    std::unique_ptr<zeromesh::node::JobManager> jm;

    explicit Requester(TestNodeFull& self) {
        auto factory = [&self](const zeromesh::net::Peer& p) {
            return std::make_shared<zeromesh::node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port),
                                                                self.id, self.pins, p.id);
        };
        zeromesh::sched::RegisterBuiltinDispatchPolicies(*self.registry);
        placer = std::make_shared<zeromesh::sched::Placer>(self.executor, self.peers, factory, self.cas, self.merkle,
                                                           self.registry, self.node_id,
                                                           [&self] { return self.telemetry; });
        jm = std::make_unique<zeromesh::node::JobManager>(
            self.cas, self.merkle, std::make_shared<zeromesh::node::ActionCache>(self.cas), placer);
    }
};

v1::JobSpec RemoteOnly(const std::string& script, v1::Digest root = {}) {
    v1::JobSpec job;
    job.set_name("job");
    auto* t = job.add_tasks();
    t->set_name("t");
    t->add_argv("sh");
    t->add_argv("-c");
    t->add_argv(script);
    *t->mutable_input_root() = std::move(root);
    t->mutable_policy()->set_may_run_local(false);
    return job;
}

std::vector<v1::JobEvent> WaitDone(zeromesh::node::JobManager& jm, const std::string& id,
                                   std::chrono::seconds limit = std::chrono::seconds(30)) {
    auto stream = jm.Watch(id);
    std::vector<v1::JobEvent> out;
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        auto ev = stream->Next(std::chrono::milliseconds(100));
        if (ev) out.push_back(*ev);
        else if (jm.Get(id)->done()) break;
    }
    return out;
}

} // namespace

TEST(PlacerRemoteOnlyRunsOnPeer) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    TempDir src;
    zeromesh::testing::WriteFile(src / "in.txt", "payload\n");
    auto root = zeromesh::node::ToProto(a.merkle->BuildTree(src.path()));
    auto job = RemoteOnly("mkdir -p out/sub && cp in.txt out/sub/copy && printf %s \"$ZM_NODE_ID\" > who && echo ran",
                          root);
    job.mutable_tasks(0)->add_outputs("who");
    job.mutable_tasks(0)->add_outputs("out");
    const std::string id = req.jm->Submit(job);
    auto events = WaitDone(*req.jm, id);

    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    const v1::TaskResult& r = st->results().at("t");
    Expect(st->tasks().at("t") == v1::SUCCEEDED, "task failed: " + r.error());
    ASSERT_EQ(r.executed_by(), std::string("node-b"));
    bool placed = false, logged = false;
    for (const auto& ev : events) {
        placed = placed || (ev.state() == v1::PLACED && ev.detail() == "node node-b");
        logged = logged || ev.log_chunk() == "ran\n";
    }
    ASSERT_TRUE(placed && logged);

    // Outputs, the whole output tree and stdout are in A's CAS.
    ASSERT_EQ(r.outputs_size(), 2);
    for (const auto& o : r.outputs()) {
        if (o.path() == "who") {
            ASSERT_EQ(a.cas->GetBlob(o.digest().hash()), std::optional<std::string>("node-b"));
        } else {
            ASSERT_TRUE(o.is_dir());
            TempDir dest;
            ASSERT_TRUE(a.merkle->MaterializeTree(zeromesh::node::FromProto(o.digest()), dest / "out"));
            ASSERT_EQ(zeromesh::testing::ReadFile(dest / "out/sub/copy"), std::string("payload\n"));
        }
    }
    ASSERT_EQ(a.cas->GetBlob(r.stdout().hash()), std::optional<std::string>("ran\n"));
}

TEST(PlacerNoCandidate) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    Requester req(a);

    std::string id = req.jm->Submit(RemoteOnly("true"));
    WaitDone(*req.jm, id);
    auto st = req.jm->Get(id);
    ASSERT_EQ(st->tasks().at("t"), v1::FAILED);
    const std::string& err = st->results().at("t").error();
    Expect(err == "no node can run this task: no eligible nodes", err);

    // A peer that rejects the offer: its reason is reported.
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    auto job = RemoteOnly("x");
    job.mutable_tasks(0)->set_argv(0, "definitely-not-a-binary-xyz");
    id = req.jm->Submit(job);
    WaitDone(*req.jm, id);
    st = req.jm->Get(id);
    ASSERT_EQ(st->tasks().at("t"), v1::FAILED);
    const std::string& err2 = st->results().at("t").error();
    Expect(err2.find("no node can run this task: node node-b: ") == 0 &&
               err2.find("not found on this node") != std::string::npos,
           err2);
}

TEST(PlacerPeerLostFailsFast) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    auto job = RemoteOnly("sleep 60");
    job.mutable_tasks(0)->mutable_policy()->set_max_retries(0);  // no re-dispatch
    const std::string id = req.jm->Submit(job);
    auto stream = req.jm->Watch(id);
    bool running = false;
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!running && std::chrono::steady_clock::now() < start_deadline) {
        auto ev = stream->Next(std::chrono::milliseconds(100));
        running = ev && ev->state() == v1::RUNNING;
    }
    ASSERT_TRUE(running);

    const auto t0 = std::chrono::steady_clock::now();
    b.Stop();
    WaitDone(*req.jm, id, std::chrono::seconds(15));
    const auto took = std::chrono::steady_clock::now() - t0;
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    ASSERT_EQ(st->tasks().at("t"), v1::FAILED);
    const std::string& err = st->results().at("t").error();
    Expect(err.find("node node-b lost during run") == 0, err);
    ASSERT_TRUE(took < std::chrono::seconds(15));
}

TEST(PlacerRetriesOnPeerLoss) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    {
        auto pb = b.AsPeer();
        pb.cpu_load_pct = 1;
        a.peers->Upsert(pb);
        auto pc = c.AsPeer();
        pc.cpu_load_pct = 50;
        a.peers->Upsert(pc);
    }
    Requester req(a);

    const std::string id = req.jm->Submit(RemoteOnly("sleep 8"));
    auto stream = req.jm->Watch(id);
    bool running = false;
    std::string placed_on;
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((!running || placed_on.empty()) && std::chrono::steady_clock::now() < start_deadline) {
        auto ev = stream->Next(std::chrono::milliseconds(100));
        if (!ev) continue;
        if (ev->state() == v1::PLACED) placed_on = ev->detail();
        if (ev->state() == v1::RUNNING) running = true;
    }
    ASSERT_TRUE(running);

    if (placed_on.find("node-b") != std::string::npos) b.Stop();
    else c.Stop();

    auto events = WaitDone(*req.jm, id, std::chrono::seconds(20));
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    Expect(st->tasks().at("t") == v1::SUCCEEDED, st->results().at("t").error());
    bool saw_retry = false;
    for (const auto& ev : events) {
        if (ev.state() == v1::QUEUED && ev.detail().find("retry 1/") == 0) saw_retry = true;
    }
    ASSERT_TRUE(saw_retry);
}

TEST(PlacerRetriesOnDonorDrain) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    {
        auto pb = b.AsPeer();
        pb.cpu_load_pct = 1;
        a.peers->Upsert(pb);
        auto pc = c.AsPeer();
        pc.cpu_load_pct = 80;
        a.peers->Upsert(pc);
    }
    Requester req(a);

    const std::string id = req.jm->Submit(RemoteOnly("sleep 8"));
    auto stream = req.jm->Watch(id);
    bool running = false;
    std::string placed_on;
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while ((!running || placed_on.empty()) && std::chrono::steady_clock::now() < start_deadline) {
        auto ev = stream->Next(std::chrono::milliseconds(100));
        if (!ev) continue;
        if (ev->state() == v1::PLACED) placed_on = ev->detail();
        if (ev->state() == v1::RUNNING) running = true;
    }
    ASSERT_TRUE(running);

    // Graceful drain of the donor that took the task (not SIGKILL).
    const std::string drained =
        placed_on.find("node-b") != std::string::npos ? std::string("node-b") : std::string("node-c");
    a.peers->MarkDraining(drained, "battery");

    auto events = WaitDone(*req.jm, id, std::chrono::seconds(20));
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    Expect(st->tasks().at("t") == v1::SUCCEEDED, st->results().at("t").error());
    bool saw_drain_retry = false;
    for (const auto& ev : events) {
        if (ev.state() == v1::QUEUED && ev.detail().find("retry 1/") == 0 &&
            ev.detail().find("donor drained") != std::string::npos) {
            saw_drain_retry = true;
        }
    }
    ASSERT_TRUE(saw_drain_retry);
}

TEST(PlacerMissingCommandNotRetried) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    auto job = RemoteOnly("x");
    job.mutable_tasks(0)->set_argv(0, "definitely-not-a-binary-xyz");
    const std::string id = req.jm->Submit(job);
    auto events = WaitDone(*req.jm, id);
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    ASSERT_EQ(st->tasks().at("t"), v1::FAILED);
    const std::string& err = st->results().at("t").error();
    Expect(err.find("not found on this node") != std::string::npos, err);
    Expect(err.find("no eligible nodes") == std::string::npos, err);
    for (const auto& ev : events) {
        ASSERT_FALSE(ev.state() == v1::QUEUED && ev.detail().find("retry ") == 0);
    }
}

TEST(PlacerTimeoutNotRetried) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    auto job = RemoteOnly("sleep 30");
    job.mutable_tasks(0)->mutable_policy()->set_timeout_s(1);
    const std::string id = req.jm->Submit(job);
    auto events = WaitDone(*req.jm, id, std::chrono::seconds(20));
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    ASSERT_EQ(st->tasks().at("t"), v1::FAILED);
    const v1::TaskResult& r = st->results().at("t");
    ASSERT_TRUE(r.timed_out());
    Expect(r.error().find("no eligible nodes") == std::string::npos, r.error());
    for (const auto& ev : events) {
        ASSERT_FALSE(ev.state() == v1::QUEUED && ev.detail().find("retry ") == 0);
    }
}

TEST(PlacerUserCancelNotRetried) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    a.peers->Upsert(b.AsPeer());
    a.peers->Upsert(c.AsPeer());
    Requester req(a);

    const std::string id = req.jm->Submit(RemoteOnly("sleep 30"));
    auto stream = req.jm->Watch(id);
    bool running = false;
    const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!running && std::chrono::steady_clock::now() < start_deadline) {
        auto ev = stream->Next(std::chrono::milliseconds(100));
        running = ev && ev->state() == v1::RUNNING;
    }
    ASSERT_TRUE(running);

    // User cancel must win even if a drain races in (no retry).
    req.jm->Cancel(id);
    a.peers->MarkDraining("node-b", "battery");
    a.peers->MarkDraining("node-c", "battery");

    auto events = WaitDone(*req.jm, id, std::chrono::seconds(15));
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    ASSERT_EQ(st->tasks().at("t"), v1::CANCELLED);
    for (const auto& ev : events) {
        ASSERT_FALSE(ev.state() == v1::QUEUED && ev.detail().find("retry ") == 0);
    }
}

TEST(PlacerInFlightCapSpread) {
    // Rank alone: with 2 total cores and 1-core tasks, a node with in_flight=2 is filtered out.
    zeromesh::v1::TaskSpec spec;
    spec.mutable_resources()->set_cores(1);
    Candidate a;
    a.node_id = "a";
    a.cores_total = 2;
    a.cores_free = 2;
    a.mem_available = 1ull << 30;
    a.arch = "x86_64";
    a.on_ac = true;
    a.in_flight = 0;
    Candidate b = a;
    b.node_id = "b";
    b.in_flight = 2;
    auto ranked = zeromesh::sched::Rank(spec, {a, b});
    ASSERT_EQ(ranked.size(), 1u);
    ASSERT_EQ(ranked[0].node_id, std::string("a"));

    // Atomic reservation: two concurrent remote-only tasks against one peer with 1 core
    // must not both stay in flight — Rank's in-flight filter uses cores_total.
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull requester("node-a", key), donor("node-b", key);
    requester.Start();
    donor.Start();
    requester.Admit(donor);
    auto peer = donor.AsPeer();
    peer.cores = 1;       // hardware capacity used by the in-flight cap
    peer.cores_free = 0;  // load spike must not change the cap
    requester.peers->Upsert(peer);
    Requester req(requester);

    v1::JobSpec job;
    job.set_name("spread");
    for (const char* name : {"t0", "t1"}) {
        auto* t = job.add_tasks();
        t->set_name(name);
        t->add_argv("sh");
        t->add_argv("-c");
        t->add_argv("sleep 2");
        t->mutable_policy()->set_may_run_local(false);
        t->mutable_resources()->set_cores(1);
    }
    const std::string id = req.jm->Submit(job);

    // While both are placing/running, in-flight on the donor must never exceed 1.
    int peak = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        peak = std::max(peak, req.placer->InFlightOn("node-b"));
        if (req.jm->Get(id)->done()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    WaitDone(*req.jm, id, std::chrono::seconds(15));
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    ASSERT_EQ(st->tasks().at("t0"), v1::SUCCEEDED);
    ASSERT_EQ(st->tasks().at("t1"), v1::SUCCEEDED);
    ASSERT_TRUE(peak <= 1);
    ASSERT_TRUE(peak >= 1);
}

namespace {

v1::JobSpec SleepJob(int n, const std::string& kind, std::map<std::string, std::string> params = {}) {
    v1::JobSpec job;
    job.set_name("sleeps");
    job.mutable_dispatch()->set_kind(kind);
    for (const auto& [k, v] : params) (*job.mutable_dispatch()->mutable_params())[k] = v;
    for (int i = 0; i < n; ++i) {
        auto* t = job.add_tasks();
        t->set_name("t" + std::to_string(i));
        t->add_argv("sh");
        t->add_argv("-c");
        t->add_argv("sleep 0.4");
        t->mutable_policy()->set_may_run_local(false);
    }
    return job;
}

// Requester + one admitted donor that announces `slots` / `queue_depth`.
struct SlotMesh {
    zeromesh::net::TeamKey key = zeromesh::net::GenerateTeamKey();
    TestNodeFull requester{"node-a", key};
    TestNodeFull donor{"node-b", key};
    std::unique_ptr<Requester> req;

    SlotMesh(int slots, int queue_depth, std::unique_ptr<zeromesh::plugin::IDispatchPolicy> extra = nullptr) {
        donor.telemetry.slots = slots;
        donor.telemetry.queue_depth = queue_depth;
        if (extra) requester.registry->RegisterDispatch(std::move(extra));
        requester.Start();
        donor.Start();
        requester.Admit(donor);
        requester.peers->Upsert(donor.AsPeer());
        req = std::make_unique<Requester>(requester);
    }

    // Peak in-flight on the donor while the job runs.
    int Run(const std::string& id) {
        int peak = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (std::chrono::steady_clock::now() < deadline && !req->jm->Get(id)->done()) {
            peak = std::max(peak, req->placer->InFlightOn("node-b"));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return peak;
    }

    void ExpectAllSucceeded(const std::string& id) {
        auto st = req->jm->Get(id);
        ASSERT_TRUE(st->done());
        for (const auto& [name, state] : st->tasks()) Expect(state == v1::SUCCEEDED, name + " did not succeed");
    }
};

} // namespace

TEST(PlacerPullRunsOnePerSlotBackToBack) {
    SlotMesh mesh(/*slots=*/1, /*queue_depth=*/0);
    const auto t0 = std::chrono::steady_clock::now();
    const std::string id = mesh.req->jm->Submit(SleepJob(4, "pull"));
    const int peak = mesh.Run(id);
    const auto took = std::chrono::steady_clock::now() - t0;
    mesh.ExpectAllSucceeded(id);
    ASSERT_EQ(peak, 1);
    // 4 × 0.4 s in series; the next task must start right after one finishes.
    Expect(took < std::chrono::milliseconds(4000), "pull left the slot idle between tasks");
}

TEST(PlacerBatchPrefetchesUpToQueueDepth) {
    SlotMesh mesh(/*slots=*/1, /*queue_depth=*/1);
    const std::string id = mesh.req->jm->Submit(SleepJob(4, "batch", {{"prefetch", "3"}}));
    const int peak = mesh.Run(id);
    mesh.ExpectAllSucceeded(id);
    ASSERT_EQ(peak, 2);  // 1 running + min(prefetch 3, queue_depth 1)
}

TEST(PlacerMaxInFlightCapsJob) {
    SlotMesh mesh(/*slots=*/4, /*queue_depth=*/0);
    const std::string id = mesh.req->jm->Submit(SleepJob(4, "fast", {{"max_in_flight", "2"}}));
    const int peak = mesh.Run(id);
    mesh.ExpectAllSucceeded(id);
    ASSERT_EQ(peak, 2);
}

TEST(PlacerUnknownDispatchPolicyFails) {
    SlotMesh mesh(1, 0);
    const std::string id = mesh.req->jm->Submit(SleepJob(1, "no-such-policy"));
    WaitDone(*mesh.req->jm, id);
    auto st = mesh.req->jm->Get(id);
    ASSERT_EQ(st->tasks().at("t0"), v1::FAILED);
    const std::string& err = st->results().at("t0").error();
    Expect(err == "unknown dispatch policy 'no-such-policy'", err);
}

namespace {

// A custom policy: one task at a time, and it records what the core reported back.
class OneAtATime final : public zeromesh::plugin::IDispatchPolicy {
public:
    std::atomic<int> decided{0};
    std::atomic<int> results{0};
    std::string_view Name() const override { return "one-at-a-time"; }
    std::vector<zeromesh::plugin::DispatchAssignment> Decide(
        const std::vector<zeromesh::plugin::DispatchTaskView>& pending,
        const std::vector<zeromesh::plugin::DispatchNodeView>& nodes, const std::map<std::string, std::string>&,
        int job_in_flight) override {
        if (job_in_flight > 0 || pending.empty()) return {};
        for (const auto& n : nodes) {
            if (!n.is_local) {
                ++decided;
                // Also try an id that is not pending; the core must ignore it.
                return {{pending.front().id, n.node_id}, {"bogus", n.node_id}};
            }
        }
        return {};
    }
    void OnResult(const std::string&, const std::string& node_id, bool ok, int64_t) override {
        if (ok && node_id == "node-b") ++results;
    }
};

} // namespace

TEST(PlacerCustomDispatchPolicyPlugin) {
    auto owned = std::make_unique<OneAtATime>();
    OneAtATime* policy = owned.get();
    SlotMesh mesh(/*slots=*/4, /*queue_depth=*/0, std::move(owned));
    const std::string id = mesh.req->jm->Submit(SleepJob(3, "one-at-a-time"));
    const int peak = mesh.Run(id);
    mesh.ExpectAllSucceeded(id);
    ASSERT_EQ(peak, 1);
    ASSERT_EQ(policy->decided.load(), 3);
    ASSERT_EQ(policy->results.load(), 3);
}
