#include "tests/unit/test_framework.hpp"

#include "plugin/loader.hpp"
#include "sched/dispatch_policy.hpp"

#include <map>
#include <string>
#include <vector>

using zeromesh::plugin::DispatchAssignment;
using zeromesh::plugin::DispatchNodeView;
using zeromesh::plugin::DispatchTaskView;

namespace {

DispatchNodeView Node(std::string id, int slots, int queue = 0, int in_flight = 0, bool local = false) {
    DispatchNodeView n;
    n.node_id = std::move(id);
    n.is_local = local;
    n.slots = slots;
    n.queue_depth = queue;
    n.in_flight = in_flight;
    n.cores_total = 8;
    n.mem_available = 1ull << 30;
    n.on_ac = true;
    n.arch = "x86_64";
    return n;
}

std::vector<DispatchTaskView> Tasks(int n) {
    std::vector<DispatchTaskView> out;
    for (int i = 0; i < n; ++i) {
        DispatchTaskView t;
        t.id = "t" + std::to_string(i);
        t.arch = "x86_64";
        out.push_back(t);
    }
    return out;
}

std::map<std::string, int> PerNode(const std::vector<DispatchAssignment>& as) {
    std::map<std::string, int> out;
    for (const auto& a : as) ++out[a.node_id];
    return out;
}

struct Policies {
    zeromesh::plugin::Registry registry;
    Policies() { zeromesh::sched::RegisterBuiltinDispatchPolicies(registry); }
    zeromesh::plugin::IDispatchPolicy& operator[](std::string_view kind) {
        auto* p = zeromesh::sched::LookupDispatchPolicy(registry, kind);
        if (!p) throw std::runtime_error("no policy " + std::string(kind));
        return *p;
    }
};

} // namespace

TEST(DispatchCapacity) {
    using zeromesh::sched::Capacity;
    ASSERT_EQ(Capacity(Node("n", 2), 1, 0), 2);
    ASSERT_EQ(Capacity(Node("n", 2, 0, 2), 1, 0), 0);
    ASSERT_EQ(Capacity(Node("n", 1, 3, 1), 1, 3), 3);
    ASSERT_EQ(Capacity(Node("n", 0), 2, 0), 4);  // no slots announced: cores_total / cores
}

TEST(DispatchPullFillsFreeSlotsOnly) {
    Policies p;
    const std::vector<DispatchNodeView> nodes{Node("a", 1), Node("b", 2), Node("c", 1, 4, 1)};
    auto as = p[""].Decide(Tasks(10), nodes, {}, 0);
    ASSERT_EQ(as.size(), 3u);  // a:1, b:2, c full (queue_depth unused by pull)
    auto per = PerNode(as);
    ASSERT_EQ(per["a"], 1);
    ASSERT_EQ(per["b"], 2);
    ASSERT_EQ(per.count("c"), 0u);
    ASSERT_EQ(as[0].task_id, std::string("t0"));  // queue order kept
}

TEST(DispatchBatchAndFastQueueAhead) {
    Policies p;
    const std::vector<DispatchNodeView> nodes{Node("a", 1, 3)};
    ASSERT_EQ(p["batch"].Decide(Tasks(10), nodes, {}, 0).size(), 2u);                  // 1 + prefetch 1
    ASSERT_EQ(p["batch"].Decide(Tasks(10), nodes, {{"prefetch", "5"}}, 0).size(), 4u);  // capped by queue_depth
    ASSERT_EQ(p["fast"].Decide(Tasks(10), nodes, {}, 0).size(), 4u);
    // A donor without queue_depth never gets more than its slots.
    ASSERT_EQ(p["fast"].Decide(Tasks(10), {Node("a", 2)}, {}, 0).size(), 2u);
}

TEST(DispatchMaxInFlightAndLocalFirst) {
    Policies p;
    const std::vector<DispatchNodeView> nodes{Node("local", 1, 0, 0, true), Node("peer", 4)};
    ASSERT_EQ(p["pull"].Decide(Tasks(10), nodes, {{"max_in_flight", "3"}}, 1).size(), 2u);
    auto as = p["local-first"].Decide(Tasks(1), nodes, {}, 0);
    ASSERT_EQ(as.size(), 1u);
    ASSERT_EQ(as[0].node_id, std::string("local"));
    ASSERT_EQ(p["pull"].Decide(Tasks(1), nodes, {}, 0)[0].node_id, std::string("peer"));  // most free slots
}

TEST(DispatchHonoursConstraintsAndExclusions) {
    Policies p;
    const std::vector<DispatchNodeView> nodes{Node("local", 1, 0, 0, true), Node("peer", 1)};
    auto tasks = Tasks(2);
    tasks[0].may_run_local = false;
    tasks[0].exclude_nodes = {"peer"};  // nowhere to go: stays queued, t1 still dispatched
    auto as = p["pull"].Decide(tasks, nodes, {}, 0);
    ASSERT_EQ(as.size(), 1u);
    ASSERT_EQ(as[0].task_id, std::string("t1"));
    ASSERT_TRUE(zeromesh::sched::LookupDispatchPolicy(p.registry, "no-such-policy") == nullptr);
}
