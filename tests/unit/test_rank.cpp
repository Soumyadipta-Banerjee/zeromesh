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
    auto ranked = Rank(spec, {Node("unknown", -1), Node("big", 5000), Node("small", 10), Node("zero", 0)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"zero", "small", "big", "unknown"}));

    // Then idle CPU, AC power, latency, and node id as the deterministic tie-break.
    Candidate busy = Node("busy", -1), battery = Node("battery", -1), far = Node("far", -1);
    busy.cpu_load_pct = 90;
    battery.on_ac = false;
    far.latency = std::chrono::milliseconds(50);
    ranked = Rank(spec, {busy, far, battery, Node("b", -1), Node("a", -1)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"a", "b", "far", "battery", "busy"}));
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
