#include "tests/unit/node_fixture.hpp"

#include "node/action_cache.hpp"
#include "node/convert.hpp"
#include "node/job_manager.hpp"
#include "sched/bandwidth.hpp"
#include "sched/placer.hpp"
#include "sched/rank.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace v1 = zeromesh::v1;
using zeromesh::sched::BandwidthEstimator;
using zeromesh::sched::Candidate;
using zeromesh::sched::Rank;
using zeromesh::sched::TransferCostSeconds;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;
using namespace std::chrono_literals;

namespace {

bool Near(double a, double b, double rel = 1e-6) {
    return std::fabs(a - b) <= rel * std::max(std::fabs(a), std::fabs(b));
}

Candidate Peer(std::string id, int64_t missing, double bandwidth = 0.0) {
    Candidate c;
    c.node_id = std::move(id);
    c.cores_total = 4;
    c.cores_free = 4;
    c.mem_available = 1ull << 30;
    c.cpu_load_pct = 10;
    c.on_ac = true;
    c.arch = "x86_64";
    c.latency = 1ms;
    c.missing_bytes = missing;
    c.bandwidth_bytes_per_s = bandwidth;
    return c;
}

std::vector<std::string> Ids(const std::vector<Candidate>& cs) {
    std::vector<std::string> out;
    for (const auto& c : cs) out.push_back(c.node_id);
    return out;
}

} // namespace

// ---- estimator -----------------------------------------------------------------------

TEST(BandwidthDefaultsBeforeAnySample) {
    BandwidthEstimator bw;
    ASSERT_TRUE(Near(bw.Estimate("node-a"), BandwidthEstimator::kDefaultBytesPerSecond));
    ASSERT_TRUE(!bw.Measured("node-a").has_value());
}

TEST(BandwidthFirstSampleIsTheMeasurement) {
    BandwidthEstimator bw;
    bw.Observe("node-a", 8'000'000, 2s); // 4 MB/s
    ASSERT_TRUE(bw.Measured("node-a").has_value());
    ASSERT_TRUE(Near(*bw.Measured("node-a"), 4'000'000.0));
    ASSERT_TRUE(Near(bw.Estimate("node-a"), 4'000'000.0));
}

TEST(BandwidthLaterSamplesBlendByEwma) {
    BandwidthEstimator bw;
    bw.Observe("node-a", 8'000'000, 2s);   // 4 MB/s
    bw.Observe("node-a", 8'000'000, 1s);   // 8 MB/s
    const double expected = BandwidthEstimator::kAlpha * 8e6 + (1.0 - BandwidthEstimator::kAlpha) * 4e6;
    ASSERT_TRUE(Near(bw.Estimate("node-a"), expected));
}

TEST(BandwidthIgnoresTransfersTooSmallToMeasureThroughput) {
    BandwidthEstimator bw;
    bw.Observe("node-a", BandwidthEstimator::kMinSampleBytes - 1, 1ms); // dominated by round-trip latency
    ASSERT_TRUE(!bw.Measured("node-a").has_value());
    bw.Observe("node-a", BandwidthEstimator::kMinSampleBytes, 1ms);
    ASSERT_TRUE(bw.Measured("node-a").has_value());
}

TEST(BandwidthIgnoresZeroOrNegativeElapsed) {
    BandwidthEstimator bw;
    bw.Observe("node-a", 10'000'000, 0ns);
    bw.Observe("node-a", 10'000'000, -5ms);
    ASSERT_TRUE(!bw.Measured("node-a").has_value());
}

TEST(BandwidthIsTrackedPerNode) {
    BandwidthEstimator bw;
    bw.Observe("fast", 100'000'000, 1s);
    bw.Observe("slow", 1'000'000, 1s);
    ASSERT_TRUE(Near(bw.Estimate("fast"), 100e6));
    ASSERT_TRUE(Near(bw.Estimate("slow"), 1e6));
    ASSERT_TRUE(Near(bw.Estimate("never-seen"), BandwidthEstimator::kDefaultBytesPerSecond));
}

// ---- cost and ranking ----------------------------------------------------------------

TEST(TransferCostIsMissingBytesOverBandwidth) {
    ASSERT_TRUE(Near(TransferCostSeconds(Peer("a", 50'000'000, 25e6), 0), 2.0));
    ASSERT_TRUE(Near(TransferCostSeconds(Peer("a", 0, 25e6), 0), 0.0) || TransferCostSeconds(Peer("a", 0, 25e6), 0) == 0.0);
    // Unknown missing bytes fall back to the caller's worst case; unknown bandwidth to the default.
    ASSERT_TRUE(Near(TransferCostSeconds(Peer("a", -1), 20'000'000), 20e6 / BandwidthEstimator::kDefaultBytesPerSecond));
}

TEST(RankPrefersFastDonorEvenWithMoreMissingBytes) {
    v1::TaskSpec spec;
    // 100 MB at 100 MB/s = 1 s; 20 MB at 1 MB/s = 20 s. Fewer bytes is not cheaper here.
    auto ranked = Rank(spec, {Peer("slow-few", 20'000'000, 1e6), Peer("fast-many", 100'000'000, 100e6)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"fast-many", "slow-few"}));
}

TEST(RankPrefersFewerBytesWhenBandwidthIsEqual) {
    v1::TaskSpec spec;
    auto ranked = Rank(spec, {Peer("many", 90'000'000, 50e6), Peer("few", 10'000'000, 50e6)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"few", "many"}));
}

TEST(RankUsesTheDefaultBandwidthForNodesNeverMeasured) {
    v1::TaskSpec spec;
    // Unmeasured nodes tie on speed, so bytes decide, as before bandwidth existed.
    auto ranked = Rank(spec, {Peer("big", 9'000'000), Peer("small", 1'000'000)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"small", "big"}));
    // A node measured slower than the default loses to an unmeasured one with more bytes.
    // 4 MB at 1 MB/s = 4 s; 10 MB at the 10 MB/s default = 1 s.
    ranked = Rank(spec, {Peer("measured-slow", 4'000'000, 1e6), Peer("unmeasured", 10'000'000)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"unmeasured", "measured-slow"}));
}

TEST(RankWeighsTransferTimeAgainstTheTasksOwnLength) {
    v1::TaskSpec spec;
    Candidate roomy = Peer("roomy", 500'000'000, 1e6); // 500 s of transfer, but more free slots
    roomy.cores_total = 16;
    Candidate snug = Peer("snug", 0, 100e6);
    snug.cores_total = 4;
    // A 60 s task would spend most of its time waiting for the input: the idle-but-ready donor wins.
    ASSERT_EQ(Ids(Rank(spec, {snug, roomy}, 0, 60.0)), (std::vector<std::string>{"snug", "roomy"}));
    // Against a task that runs for hours the same 500 s is noise, so the extra slots decide.
    ASSERT_EQ(Ids(Rank(spec, {snug, roomy}, 0, 36'000.0)), (std::vector<std::string>{"roomy", "snug"}));
}

TEST(RankTreatsAnAlreadyHeldInputAsFreeWhateverTheLink) {
    v1::TaskSpec spec;
    auto ranked = Rank(spec, {Peer("needs-bytes", 1'000, 100e6), Peer("has-it", 0, 1e3)});
    ASSERT_EQ(Ids(ranked), (std::vector<std::string>{"has-it", "needs-bytes"}));
}

// ---- placer measures it from real transfers -------------------------------------------

TEST(PlacerMeasuresDonorBandwidthFromRealPushes) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());

    auto factory = [&a](const zeromesh::net::Peer& p) {
        return std::make_shared<zeromesh::node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port), a.id,
                                                            a.pins, p.id);
    };
    auto placer = std::make_shared<zeromesh::sched::Placer>(a.executor, a.peers, factory, a.cas, a.merkle, a.node_id,
                                                            [&a] { return a.telemetry; });
    zeromesh::node::JobManager jm(a.cas, a.merkle, std::make_shared<zeromesh::node::ActionCache>(a.cas), placer);

    TempDir src;
    zeromesh::testing::WriteFile(src / "big.bin", std::string(400'000, 'z')); // well above kMinSampleBytes
    v1::JobSpec job;
    job.set_name("bw");
    auto* t = job.add_tasks();
    t->set_name("t");
    t->add_argv("sh");
    t->add_argv("-c");
    t->add_argv("wc -c < big.bin");
    *t->mutable_input_root() = zeromesh::node::ToProto(a.merkle->BuildTree(src.path()));
    t->mutable_policy()->set_may_run_local(false);

    ASSERT_TRUE(!placer->Bandwidth().Measured("node-b").has_value());
    const std::string id = jm.Submit(job);
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!jm.Get(id)->done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
    auto st = jm.Get(id);
    ASSERT_TRUE(st->done());
    ASSERT_TRUE(placer->Traffic().at("node-b").bytes_pushed >= 400'000);

    auto measured = placer->Bandwidth().Measured("node-b");
    ASSERT_TRUE(measured.has_value());
    ASSERT_TRUE(*measured > 0.0);
    ASSERT_TRUE(std::isfinite(*measured));
}
