#include "tests/unit/test_framework.hpp"

#include "sched/rank.hpp"

#include <vector>

using zeromesh::sched::Candidate;
using zeromesh::sched::Rank;

namespace {

Candidate Node(std::string id, int64_t missing, bool local = false) {
    Candidate c;
    c.node_id = std::move(id);
    c.is_local = local;
    c.cores_total = 4;
    c.cores_free = 4;
    c.mem_available = 1ull << 30;
    c.cpu_load_pct = 10;
    c.on_ac = true;
    c.arch = "x86_64";
    c.latency = std::chrono::milliseconds(1);
    c.missing_bytes = missing;
    return c;
}

std::vector<std::string> Ids(const std::vector<Candidate>& cs) {
    std::vector<std::string> out;
    for (const auto& c : cs) out.push_back(c.node_id);
    return out;
}

} // namespace

TEST(RankPrefersFewerMissingBytes) {
    zeromesh::v1::TaskSpec spec;
    // Same free slots → missing_bytes breaks the tie.
    auto ranked = Rank(spec, {Node("unknown", -1), Node("big", 5000), Node("small", 10), Node("zero", 0)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"zero", "small", "big", "unknown"}));

    Candidate busy = Node("busy", -1), battery = Node("battery", -1), far = Node("far", -1);
    busy.cpu_load_pct = 90;
    battery.on_ac = false;
    far.latency = std::chrono::milliseconds(50);
    ranked = Rank(spec, {busy, far, battery, Node("b", -1), Node("a", -1)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"a", "b", "far", "battery", "busy"}));
}

TEST(RankPrefersMoreFreeSlots) {
    zeromesh::v1::TaskSpec spec;
    Candidate local = Node("local", 0, true);
    local.cores_total = 12;
    local.cores_free = 12;
    local.in_flight = 0;
    Candidate peer = Node("peer", -1);
    peer.cores_total = 20;
    peer.cores_free = 20;
    peer.in_flight = 0;
    // Peer has more free slots despite unknown missing_bytes — do not pack local first.
    ASSERT_EQ(Ids(Rank(spec, {local, peer})), (std::vector<std::string>{"peer", "local"}));

    peer.in_flight = 10;  // 10 slots left on peer, 12 on local
    ASSERT_EQ(Ids(Rank(spec, {local, peer})), (std::vector<std::string>{"local", "peer"}));
}

TEST(RankIgnoresLiveCoresFreeWhenBusy) {
    // A machine at 100% CPU reports cores_free≈0; capacity must still use cores_total.
    zeromesh::v1::TaskSpec spec;
    spec.mutable_resources()->set_cores(8);
    Candidate busy = Node("busy", 0);
    busy.cores_total = 20;
    busy.cores_free = 0;
    busy.cpu_load_pct = 100;
    busy.in_flight = 1;  // one of two slots used (20/8=2)
    ASSERT_EQ(Ids(Rank(spec, {busy})), (std::vector<std::string>{"busy"}));
    busy.in_flight = 2;
    ASSERT_TRUE(Rank(spec, {busy}).empty());
}

TEST(RankFiltersArchAndRemoteOnly) {
    zeromesh::v1::TaskSpec spec;
    Candidate arm = Node("arm", 0);
    arm.arch = "aarch64";
    Candidate full = Node("full", 0);
    full.in_flight = 4;  // 4 free cores, 4 tasks of 1 core in flight
    Candidate small = Node("small-mem", 0);
    small.mem_available = 100;
    std::vector<Candidate> all{Node("local", 0, true), Node("peer", 100), arm, full, small};

    // Empty arch = the local node's arch.
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"local", "small-mem", "peer"}));
    spec.mutable_resources()->set_mem_bytes(1000);
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"local", "peer"}));
    spec.mutable_policy()->set_may_run_local(false);
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"peer"}));
    spec.mutable_policy()->set_may_run_local(true);
    spec.mutable_policy()->set_may_run_remote(false);
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"local"}));
    spec.mutable_policy()->set_may_run_remote(true);
    spec.mutable_resources()->set_arch("aarch64");
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"arm"}));
    spec.mutable_resources()->set_arch("");
    spec.mutable_resources()->set_cores(5);
    ASSERT_TRUE(Rank(spec, all).empty());
}

TEST(RankInFlightCapUsesCoresTotal) {
    // At most cores_total / task.cores tasks in flight per node.
    zeromesh::v1::TaskSpec spec;
    spec.mutable_resources()->set_cores(2);
    Candidate n;
    n.node_id = "n";
    n.cores_total = 4;
    n.cores_free = 4;
    n.mem_available = 1ull << 30;
    n.arch = "x86_64";
    n.on_ac = true;
    n.in_flight = 1;  // 4 - 1*2 = 2 >= 2: still room
    ASSERT_EQ(Rank(spec, {n}).size(), 1u);
    n.in_flight = 2;  // 4 - 2*2 = 0 < 2: full
    ASSERT_TRUE(Rank(spec, {n}).empty());
}


TEST(RankCapacityIgnoresLoadDerivedCoresFree) {
    // Root cause of flaky local multi-task jobs: cores_free = round(cores_total*(1-load))
    // can be 0 under brief 100% load. That must not make a node ineligible — idle is a
    // ranking signal (cpu_load_pct), capacity is cores_total.
    zeromesh::v1::TaskSpec spec;
    Candidate hot;
    hot.node_id = "hot";
    hot.cores_total = 8;
    hot.cores_free = 0;  // fully loaded by telemetry
    hot.cpu_load_pct = 100;
    hot.mem_available = 1ull << 30;
    hot.arch = "x86_64";
    hot.on_ac = true;
    hot.in_flight = 0;
    ASSERT_EQ(Rank(spec, {hot}).size(), 1u);

    hot.in_flight = 8;  // saturated on hardware capacity
    ASSERT_TRUE(Rank(spec, {hot}).empty());

    Candidate idle = hot;
    idle.node_id = "idle";
    idle.cores_free = 8;
    idle.cpu_load_pct = 5;
    idle.in_flight = 0;
    hot.in_flight = 0;
    // Both eligible; idle ranks first.
    ASSERT_EQ(Ids(Rank(spec, {hot, idle})), (std::vector<std::string>{"idle", "hot"}));
}
