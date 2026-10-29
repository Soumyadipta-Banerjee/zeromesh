#include "tests/unit/node_fixture.hpp"

#include "node/convert.hpp"
#include "node/donor_policy.hpp"
#include "sched/placer.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace v1 = zeromesh::v1;
using zeromesh::testing::TestNodeFull;
using namespace std::chrono_literals;

namespace {

void HExpect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

// A placer over `self`, and the batches handed to it (one remote-only item each).
struct HedgeRequester {
    // Declared first so they outlive the placer: its workers report into them until it is destroyed.
    std::vector<std::unique_ptr<zeromesh::testing::AttemptLog>> logs;
    std::shared_ptr<zeromesh::sched::Placer> placer;

    explicit HedgeRequester(TestNodeFull& self) {
        auto factory = [&self](const zeromesh::net::Peer& p) {
            return std::make_shared<zeromesh::node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port),
                                                                self.id, self.pins, p.id);
        };
        placer = std::make_shared<zeromesh::sched::Placer>(self.executor, self.peers, factory, self.cas, self.merkle,
                                                           self.node_id, [&self] { return self.telemetry; });
    }
    void Submit(int n, const std::string& script) {
        for (int i = 0; i < n; ++i) {
            logs.push_back(std::make_unique<zeromesh::testing::AttemptLog>());
            placer->Execute("run/b" + std::to_string(i), zeromesh::testing::ShBatch({{"t" + std::to_string(i), script}}),
                            {}, logs.back()->Callbacks());
        }
    }
    bool Done(std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        for (auto& l : logs) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (!l->Wait(std::max(left, std::chrono::milliseconds(1)))) return false;
        }
        return true;
    }
    bool Ok(size_t i) { return logs[i]->result.failure == v1::FAILURE_NONE && !logs[i]->items.empty(); }
    std::string Node(size_t i) { return logs[i]->items.empty() ? "" : logs[i]->items[0].second; }
    std::string Message(size_t i) { return logs[i]->result.message; }
};

// A hedge config that reacts fast and, unless a test overrides it, hedges in a small tail.
zeromesh::sched::Placer::HedgeConfig Eager(std::function<double(const std::string&, double)> survival) {
    zeromesh::sched::Placer::HedgeConfig c;
    c.survival = std::move(survival);
    c.survive_below = 0.5;
    c.max_copies = 2;
    c.tail_max_running = 4;
    c.check_interval = 50ms;
    c.retry_after = 300ms;
    return c;
}

bool WaitUntil(const std::function<bool()>& pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

} // namespace

TEST(HedgeFinishesOnSurvivorWhenPrimaryDonorDies) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), d1("node-d1", key), d2("node-d2", key);
    a.Start();
    d1.Start();
    d2.Start();
    a.Admit(d1);
    a.Admit(d2);
    a.peers->Upsert(d1.AsPeer()); // d2 joins later, so the primary is certain to land on d1
    HedgeRequester req(a);
    req.placer->SetHedging(Eager([](const std::string& node, double) { return node == "node-d1" ? 0.1 : 0.99; }));

    req.Submit(1, "sleep 2; echo ok");
    ASSERT_TRUE(WaitUntil([&] { return req.placer->InFlightOn("node-d1") == 1; }, 10s));
    a.peers->Upsert(d2.AsPeer());
    ASSERT_TRUE(WaitUntil([&] { return req.placer->Hedging().launched == 1; }, 10s));

    d1.Stop(); // the primary's donor disappears while the hedge is running

    ASSERT_TRUE(req.Done(30s));
    HExpect(req.Ok(0), req.Message(0));
    ASSERT_EQ(req.Node(0), std::string("node-d2"));
    ASSERT_EQ(req.placer->Hedging().won, static_cast<uint64_t>(1));
}

TEST(HedgeDisabledWithoutASurvivalFunction) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), d1("node-d1", key), d2("node-d2", key);
    a.Start();
    d1.Start();
    d2.Start();
    a.Admit(d1);
    a.Admit(d2);
    a.peers->Upsert(d1.AsPeer());
    a.peers->Upsert(d2.AsPeer());
    HedgeRequester req(a);
    req.placer->SetHedging(Eager(nullptr)); // no hazard model: never hedge

    req.Submit(1, "sleep 1");
    std::this_thread::sleep_for(600ms);
    ASSERT_EQ(req.placer->Hedging().launched, static_cast<uint64_t>(0));
    ASSERT_TRUE(req.Done(30s));
}

TEST(HedgeNotLaunchedOutsideTheTail) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), d1("node-d1", key), d2("node-d2", key);
    a.Start();
    d1.Start();
    d2.Start();
    a.Admit(d1);
    a.Admit(d2);
    a.peers->Upsert(d1.AsPeer());
    a.peers->Upsert(d2.AsPeer());
    HedgeRequester req(a);
    auto cfg = Eager([](const std::string&, double) { return 0.0; }); // every donor looks doomed
    cfg.tail_max_running = 2;
    req.placer->SetHedging(cfg);

    req.Submit(6, "sleep 2");
    // Six tasks are running: far from the tail, so no hedge although every survival is 0.
    std::this_thread::sleep_for(700ms);
    ASSERT_EQ(req.placer->Hedging().launched, static_cast<uint64_t>(0));
    ASSERT_TRUE(req.Done(60s));
}

TEST(HedgeNotLaunchedWhileWorkWaitsForASlot) {
    // By the placer's count this is the tail (one batch, running), but the run engine still has
    // items waiting for a slot: a copy would take theirs.
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), d1("node-d1", key), d2("node-d2", key);
    a.Start();
    d1.Start();
    d2.Start();
    a.Admit(d1);
    a.Admit(d2);
    a.peers->Upsert(d1.AsPeer()); // the primary lands on d1
    std::atomic<bool> backlog{true}; // outlives the placer, whose hedge thread reads it
    HedgeRequester req(a);
    auto cfg = Eager([](const std::string& node, double) { return node == "node-d1" ? 0.1 : 0.99; });
    cfg.backlog = [&backlog] { return backlog.load(); };
    req.placer->SetHedging(cfg);

    req.Submit(1, "sleep 3; echo ok");
    ASSERT_TRUE(WaitUntil([&] { return req.placer->InFlightOn("node-d1") == 1; }, 10s));
    a.peers->Upsert(d2.AsPeer());
    std::this_thread::sleep_for(600ms);
    ASSERT_EQ(req.placer->Hedging().launched, static_cast<uint64_t>(0));
    backlog = false; // nothing waits any more: the tail
    ASSERT_TRUE(WaitUntil([&] { return req.placer->Hedging().launched == 1; }, 10s));
    ASSERT_TRUE(req.Done(60s));
}

TEST(HedgeNeverExceedsMaxCopies) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), d1("node-d1", key), d2("node-d2", key), d3("node-d3", key);
    a.Start();
    d1.Start();
    d2.Start();
    d3.Start();
    a.Admit(d1);
    a.Admit(d2);
    a.Admit(d3);
    a.peers->Upsert(d1.AsPeer());
    a.peers->Upsert(d2.AsPeer());
    a.peers->Upsert(d3.AsPeer());
    HedgeRequester req(a);
    req.placer->SetHedging(Eager([](const std::string&, double) { return 0.0; }));

    auto copies = [&] {
        return req.placer->InFlightOn("node-d1") + req.placer->InFlightOn("node-d2") +
               req.placer->InFlightOn("node-d3");
    };
    req.Submit(1, "sleep 4");
    ASSERT_TRUE(WaitUntil([&] { return req.placer->Hedging().launched == 1; }, 10s));
    // Several more scan intervals pass; the cap of 2 copies holds.
    std::this_thread::sleep_for(1s);
    ASSERT_EQ(req.placer->Hedging().launched, static_cast<uint64_t>(1));
    ASSERT_EQ(copies(), 2);
    ASSERT_TRUE(req.Done(30s));
}

TEST(HedgeMaxCopiesThreeAllowsTwoHedges) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), d1("node-d1", key), d2("node-d2", key), d3("node-d3", key);
    a.Start();
    d1.Start();
    d2.Start();
    d3.Start();
    a.Admit(d1);
    a.Admit(d2);
    a.Admit(d3);
    a.peers->Upsert(d1.AsPeer());
    a.peers->Upsert(d2.AsPeer());
    a.peers->Upsert(d3.AsPeer());
    HedgeRequester req(a);
    auto cfg = Eager([](const std::string&, double) { return 0.0; });
    cfg.max_copies = 3;
    req.placer->SetHedging(cfg);

    req.Submit(1, "sleep 4");
    ASSERT_TRUE(WaitUntil([&] { return req.placer->Hedging().launched == 2; }, 10s));
    std::this_thread::sleep_for(1s);
    ASSERT_EQ(req.placer->Hedging().launched, static_cast<uint64_t>(2));
    ASSERT_TRUE(req.Done(30s));
}

TEST(HedgeLoserIsCancelledAndItsCpuIsCounted) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), fast("node-fast", key), slow("node-slow", key);
    a.Start();
    fast.Start();
    slow.Start();
    a.Admit(fast);
    a.Admit(slow);
    a.peers->Upsert(fast.AsPeer()); // the primary lands on `fast`; `slow` joins afterwards
    HedgeRequester req(a);
    // `fast` looks doomed, so the task is copied to `slow`, which then loses the race.
    req.placer->SetHedging(Eager([](const std::string& node, double) { return node == "node-fast" ? 0.1 : 0.99; }));

    // 2 s on `fast`, so the hedge is certain to be running before the original finishes.
    const std::string script = "if [ \"$ZM_NODE_ID\" = node-fast ]; then sleep 2; else sleep 30; fi";
    req.Submit(1, script);
    ASSERT_TRUE(WaitUntil([&] { return req.placer->InFlightOn("node-fast") == 1; }, 10s));
    a.peers->Upsert(slow.AsPeer());

    ASSERT_TRUE(req.Done(20s));
    HExpect(req.Ok(0), req.Message(0));
    ASSERT_EQ(req.Node(0), std::string("node-fast"));

    // The hedge copy on `slow` is told to stop; its slot frees up instead of running 30 s.
    ASSERT_TRUE(WaitUntil([&] { return req.placer->InFlightOn("node-slow") == 0; }, 10s));
    ASSERT_TRUE(WaitUntil([&] { return req.placer->Hedging().cancelled == 1; }, 10s));
    const auto h = req.placer->Hedging();
    ASSERT_EQ(h.launched, static_cast<uint64_t>(1));
    ASSERT_EQ(h.won, static_cast<uint64_t>(0));
    // Only the hedge's own run time is charged, and all of it is waste because it lost.
    HExpect(h.cpu_seconds > 0.2 && h.cpu_seconds < 10.0, "cpu_seconds " + std::to_string(h.cpu_seconds));
    ASSERT_TRUE(h.wasted_cpu_seconds > 0.2);
    ASSERT_TRUE(h.wasted_cpu_seconds <= h.cpu_seconds + 1e-9);
}

TEST(HedgeWinnerCpuIsCountedButNotWasted) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), slow("node-slow", key), fast("node-fast", key);
    a.Start();
    slow.Start();
    fast.Start();
    a.Admit(slow);
    a.Admit(fast);
    a.peers->Upsert(slow.AsPeer());
    HedgeRequester req(a);
    req.placer->SetHedging(Eager([](const std::string& node, double) { return node == "node-slow" ? 0.1 : 0.99; }));

    const std::string script = "if [ \"$ZM_NODE_ID\" = node-fast ]; then sleep 1; else sleep 30; fi";
    req.Submit(1, script);
    ASSERT_TRUE(WaitUntil([&] { return req.placer->InFlightOn("node-slow") == 1; }, 10s));
    a.peers->Upsert(fast.AsPeer());

    ASSERT_TRUE(req.Done(20s));
    HExpect(req.Ok(0), req.Message(0));
    ASSERT_EQ(req.Node(0), std::string("node-fast"));
    // The original (slow) copy was cancelled once the hedge won.
    ASSERT_TRUE(WaitUntil([&] { return req.placer->InFlightOn("node-slow") == 0; }, 10s));
    const auto h = req.placer->Hedging();
    ASSERT_EQ(h.won, static_cast<uint64_t>(1));
    ASSERT_TRUE(h.cpu_seconds > 0.5);
    ASSERT_EQ(h.wasted_cpu_seconds, 0.0);
}

TEST(HedgeConfigFollowsDonorPolicyMaxCopies) {
    std::ostringstream warn;
    auto p = zeromesh::node::DonorPolicy::Parse("max_copies = 3\n", warn);
    ASSERT_EQ(p.max_copies, 3);
    ASSERT_EQ(warn.str(), std::string(""));
    ASSERT_EQ(zeromesh::node::DonorPolicy{}.max_copies, 2);
    auto cfg = zeromesh::sched::Placer::HedgeConfig::FromPolicy(p);
    ASSERT_EQ(cfg.max_copies, 3);
    // Nothing to hedge with until a hazard model supplies survival odds.
    ASSERT_TRUE(!cfg.survival);
}
