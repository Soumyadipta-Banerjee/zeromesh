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
    c.work_cores = 4;
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
    zeromesh::sched::Need spec;
    // Same free slots → missing_bytes breaks the tie (-1 ranks as worst-case input size).
    constexpr int64_t kInput = 10000;
    auto ranked = Rank(spec, {Node("unknown", -1), Node("big", 5000), Node("small", 10), Node("zero", 0)}, kInput);
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"zero", "small", "big", "unknown"}));

    Candidate busy = Node("busy", -1), battery = Node("battery", -1), far = Node("far", -1);
    busy.cpu_load_pct = 90;
    battery.on_ac = false;
    far.latency = std::chrono::milliseconds(50);
    ranked = Rank(spec, {busy, far, battery, Node("b", -1), Node("a", -1)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"a", "b", "far", "battery", "busy"}));
}

TEST(RankPrefersMoreFreeSlots) {
    zeromesh::sched::Need spec;
    Candidate local = Node("local", 0, true);
    local.work_cores = 12;
    Candidate peer = Node("peer", -1);
    peer.work_cores = 20;
    // Peer has more free slots despite unknown missing_bytes — do not pack local first.
    ASSERT_EQ(Ids(Rank(spec, {local, peer}, 1'000'000)), (std::vector<std::string>{"peer", "local"}));

    peer.cores_in_use = 10; // 10 slots left on peer, 12 on local
    ASSERT_EQ(Ids(Rank(spec, {local, peer}, 1'000'000)), (std::vector<std::string>{"local", "peer"}));
}

TEST(RankIdleRemoteBeatsBusyLocalOnSmallInput) {
    zeromesh::sched::Need spec;
    Candidate local = Node("local", 0, true);
    local.work_cores = 8;
    local.cores_in_use = 7; // one slot left
    Candidate remote = Node("remote", -1);
    remote.work_cores = 8; // eight slots
    ASSERT_EQ(Ids(Rank(spec, {local, remote}, 512)), (std::vector<std::string>{"remote", "local"}));
}

TEST(RankHugeInputPrefersNodeWithInputs) {
    zeromesh::sched::Need spec;
    constexpr int64_t kHuge = 50'000'000'000LL;
    Candidate local = Node("local", 0, true);
    Candidate cold = Node("cold", -1);
    Candidate warm = Node("warm", 0);
    // Equal capacity; huge worst-case transfer cost should not beat a peer that already has the tree.
    ASSERT_EQ(Ids(Rank(spec, {local, cold, warm}, kHuge)), (std::vector<std::string>{"local", "warm", "cold"}));
}

TEST(RankFiltersArchAndRemoteOnly) {
    zeromesh::sched::Need spec;
    Candidate arm = Node("arm", 0);
    arm.arch = "aarch64";
    Candidate full = Node("full", 0);
    full.cores_in_use = 4; // all 4 cores held
    Candidate small = Node("small-mem", 0);
    small.mem_available = 100;
    std::vector<Candidate> all{Node("local", 0, true), Node("peer", 100), arm, full, small};

    // Empty arch = the local node's arch.
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"local", "small-mem", "peer"}));
    spec.mem_bytes = 1000;
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"local", "peer"}));
    spec.may_local = false;
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"peer"}));
    spec.may_local = true;
    spec.may_remote = false;
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"local"}));
    spec.may_remote = true;
    spec.arch = "aarch64";
    ASSERT_EQ(Ids(Rank(spec, all)), (std::vector<std::string>{"arm"}));
    spec.arch = "";
    spec.cores = 5;
    ASSERT_TRUE(Rank(spec, all).empty());
}

TEST(RankFreeSlotsAreWorkCoresLessCoresInUse) {
    // A node takes batches up to the cores it lets work hold, less what is held there already.
    zeromesh::sched::Need spec;
    spec.cores = 4;
    Candidate c = Node("donor", 0);
    c.work_cores = 2; // not even one 4-core batch fits
    ASSERT_TRUE(Rank(spec, {c}).empty());
    c.work_cores = 8;
    c.cores_in_use = 4; // one more 4-core batch
    ASSERT_EQ(Ids(Rank(spec, {c})), (std::vector<std::string>{"donor"}));
    c.cores_in_use = 6; // 2 cores left: not enough
    ASSERT_TRUE(Rank(spec, {c}).empty());
}

TEST(RankCapacityIgnoresLoad) {
    // A machine its owner keeps at 100% CPU still has the cores it lets work hold: load ranks
    // (cpu_load_pct), it does not make the node look gone.
    zeromesh::sched::Need spec;
    Candidate hot = Node("hot", 0);
    hot.work_cores = 8;
    hot.cpu_load_pct = 100;
    ASSERT_EQ(Rank(spec, {hot}).size(), 1u);
    Candidate idle = hot;
    idle.node_id = "idle";
    idle.cpu_load_pct = 5;
    ASSERT_EQ(Ids(Rank(spec, {hot, idle})), (std::vector<std::string>{"idle", "hot"}));
}

// ---- liveput: slots x E[min(remaining life, task time)] (Parcae-style) ----

namespace {

Candidate WithLife(Candidate c, double expected_min_s) {
    c.expected_min_s = expected_min_s;
    return c;
}

} // namespace

TEST(RankFlakyDonorLosesToStableDonorWithEqualCapacity) {
    zeromesh::sched::Need spec;
    // Same cores, and the flaky one even holds all the inputs already; it still loses because
    // little of what it is given will ever finish.
    Candidate flaky = WithLife(Node("flaky", 0), 60.0);
    Candidate stable = WithLife(Node("stable", 5'000'000), 600.0);
    ASSERT_EQ(Ids(Rank(spec, {flaky, stable}, 1000)), (std::vector<std::string>{"stable", "flaky"}));
    ASSERT_EQ(Ids(Rank(spec, {stable, flaky}, 1000)), (std::vector<std::string>{"stable", "flaky"}));
}

TEST(RankLiveputWeighsSlotsTimesExpectedLife) {
    zeromesh::sched::Need spec;
    Candidate big_flaky = WithLife(Node("big-flaky", 0), 100.0); // 16 slots x 100 = 1600
    big_flaky.work_cores = 16;
    Candidate small_stable = WithLife(Node("small-stable", 0), 600.0); // 4 x 600 = 2400
    ASSERT_EQ(Ids(Rank(spec, {big_flaky, small_stable})), (std::vector<std::string>{"small-stable", "big-flaky"}));

    big_flaky.expected_min_s = 300.0; // 16 x 300 = 4800 now beats 2400
    ASSERT_EQ(Ids(Rank(spec, {big_flaky, small_stable})), (std::vector<std::string>{"big-flaky", "small-stable"}));
}

TEST(RankNoHistoryKeepsTheExistingOrder) {
    zeromesh::sched::Need spec;
    Candidate local = Node("local", 0, true);
    local.work_cores = 12;
    Candidate peer = Node("peer", -1);
    peer.work_cores = 20;
    Candidate near_peer = Node("near", 10);
    Candidate far_peer = Node("far", 5000);
    std::vector<Candidate> all{far_peer, local, near_peer, peer};
    const auto baseline = Ids(Rank(spec, all, 10000));
    ASSERT_EQ(baseline, (std::vector<std::string>{"peer", "local", "near", "far"}));

    // All unknown (the default, -1): identical.
    for (auto& c : all) c.expected_min_s = -1.0;
    ASSERT_EQ(Ids(Rank(spec, all, 10000)), baseline);
    // All equally known: still identical, whatever the common value.
    for (auto& c : all) c.expected_min_s = 321.0;
    ASSERT_EQ(Ids(Rank(spec, all, 10000)), baseline);
}

TEST(RankUnknownLifeCountsAsTheAverageOfTheKnown) {
    zeromesh::sched::Need spec;
    // One known candidate and one unknown: the unknown one is neutral, so the old
    // missing-bytes order decides.
    Candidate known = WithLife(Node("known", 900), 300.0);
    Candidate unknown = Node("unknown", 0);
    ASSERT_EQ(Ids(Rank(spec, {known, unknown})), (std::vector<std::string>{"unknown", "known"}));
}

TEST(RankNearlyEqualLiveputFallsBackToMissingBytes) {
    zeromesh::sched::Need spec;
    // 150 s versus 151 s of expected life is noise; the cheaper transfer wins.
    Candidate warm = WithLife(Node("warm", 0), 150.0);
    Candidate cold = WithLife(Node("cold", 500), 151.0);
    ASSERT_EQ(Ids(Rank(spec, {cold, warm})), (std::vector<std::string>{"warm", "cold"}));
}

TEST(RankMissingInputOutweighsExtraSlots) {
    zeromesh::sched::Need spec;
    // Five more slots do not make up for 5 GB that must cross a 10 MB/s link before a 60 s task starts.
    Candidate cold = Node("cold", 5'000'000'000LL);
    cold.work_cores = 12;
    Candidate warm = Node("warm", 0);
    warm.work_cores = 7;
    ASSERT_EQ(Ids(Rank(spec, {cold, warm}, 0, 60.0)), (std::vector<std::string>{"warm", "cold"}));
    // The same transfer is nothing against a task that runs for hours, so the slots decide.
    ASSERT_EQ(Ids(Rank(spec, {cold, warm}, 0, 100'000.0)), (std::vector<std::string>{"cold", "warm"}));
}

TEST(RankUsesMeasuredBandwidthForTransferCost) {
    zeromesh::sched::Need spec;
    Candidate slow = Node("slow", 200'000'000);
    slow.work_cores = 8;
    slow.bandwidth_bytes_per_s = 1'000'000.0; // 200 s
    Candidate fast = Node("fast", 200'000'000);
    fast.work_cores = 6;
    fast.bandwidth_bytes_per_s = 100'000'000.0; // 2 s
    ASSERT_EQ(Ids(Rank(spec, {slow, fast}, 0, 60.0)), (std::vector<std::string>{"fast", "slow"}));
}

TEST(RankHoldsCommittedMemoryAgainstTheDonorTotal) {
    zeromesh::sched::Need spec;
    spec.mem_bytes = 4ll << 30;
    Candidate c = Node("donor", 0);
    c.work_cores = 16;
    c.mem_total = 16ull << 30;
    c.mem_available = 16ull << 30; // tasks placed earlier have not touched their memory yet
    c.mem_committed = 12ll << 30;
    ASSERT_EQ(Ids(Rank(spec, {c})), (std::vector<std::string>{"donor"})); // exactly 4 GB left
    c.mem_committed = 13ll << 30;
    ASSERT_TRUE(Rank(spec, {c}).empty());
    c.mem_total = 0; // total unknown: the commitment cannot be judged and is ignored
    ASSERT_EQ(Ids(Rank(spec, {c})), (std::vector<std::string>{"donor"}));
}
