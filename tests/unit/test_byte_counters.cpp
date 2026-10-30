#include "tests/unit/node_fixture.hpp"

#include "node/action_cache.hpp"
#include "node/convert.hpp"
#include "node/job_manager.hpp"
#include "sched/placer.hpp"

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace v1 = zeromesh::v1;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;

namespace {

void Expect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

struct Requester {
    std::shared_ptr<zeromesh::sched::Placer> placer;
    std::unique_ptr<zeromesh::node::JobManager> jm;

    explicit Requester(TestNodeFull& self) {
        auto factory = [&self](const zeromesh::net::Peer& p) {
            return std::make_shared<zeromesh::node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port),
                                                                self.id, self.pins, p.id);
        };
        placer = std::make_shared<zeromesh::sched::Placer>(self.executor, self.peers, factory, self.cas, self.merkle,
                                                           self.node_id, [&self] { return self.telemetry; });
        jm = std::make_unique<zeromesh::node::JobManager>(
            self.cas, self.merkle, std::make_shared<zeromesh::node::ActionCache>(self.cas), placer);
    }
};

void WaitJob(zeromesh::node::JobManager& jm, const std::string& id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        if (jm.Get(id)->done()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("job " + id + " did not finish");
}

// One task per case; every case reads the shared tree plus its own `case_<i>` file.
v1::JobSpec Sweep(const std::vector<v1::Digest>& roots) {
    v1::JobSpec job;
    job.set_name("sweep");
    for (size_t i = 0; i < roots.size(); ++i) {
        auto* t = job.add_tasks();
        t->set_name("case" + std::to_string(i));
        t->add_argv("sh");
        t->add_argv("-c");
        t->add_argv("cat case.txt shared.bin | wc -c");
        *t->mutable_input_root() = roots[i];
        t->mutable_policy()->set_may_run_local(false);
    }
    return job;
}

std::string Blob(size_t n, char c) { return std::string(n, c); }

} // namespace

TEST(NodeClientReportsBytesOnTheWire) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);

    TempDir src;
    zeromesh::testing::WriteFile(src / "in.bin", Blob(100000, 'x'));
    auto root = a.merkle->BuildTree(src.path());
    std::vector<v1::Digest> digests;
    uint64_t expected = 0;
    for (const auto& d : a.merkle->CollectTree(root)) {
        digests.push_back(zeromesh::node::ToProto(d));
        expected += static_cast<uint64_t>(d.size_bytes);
    }

    auto client = a.ClientTo(b);
    std::string err;
    uint64_t pushed = 0;
    Expect(client->Push(*a.cas, digests, &err, &pushed), err);
    ASSERT_EQ(pushed, expected);

    // Pulling into a fresh store reports the same payload size.
    TempDir other;
    zeromesh::store::CasStore fresh(other / "cas");
    uint64_t pulled = 0;
    Expect(client->Pull(fresh, digests, &err, &pulled), err);
    ASSERT_EQ(pulled, expected);
}

TEST(PlacerSendsSharedInputOncePerDonor) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    constexpr int kCases = 6;
    TempDir src;
    zeromesh::testing::WriteFile(src / "shared.bin", Blob(200000, 's'));
    std::vector<v1::Digest> roots;
    uint64_t one_tree = 0;
    for (int i = 0; i < kCases; ++i) {
        zeromesh::testing::WriteFile(src / "case.txt", "case " + std::to_string(i) + "\n");
        auto root = a.merkle->BuildTree(src.path());
        roots.push_back(zeromesh::node::ToProto(root));
    }
    // What a donor needs for all cases together: each distinct blob once.
    std::set<std::string> seen;
    for (const auto& r : roots) {
        for (const auto& d : a.merkle->CollectTree(zeromesh::node::FromProto(r))) {
            if (seen.insert(d.hash).second) one_tree += static_cast<uint64_t>(d.size_bytes);
        }
    }

    const std::string id = req.jm->Submit(Sweep(roots));
    WaitJob(*req.jm, id);
    auto st = req.jm->Get(id);
    for (const auto& [name, state] : st->tasks()) {
        Expect(state == v1::SUCCEEDED, name + ": " + st->results().at(name).error());
    }

    const auto traffic = req.placer->Traffic();
    ASSERT_EQ(traffic.count("node-b"), static_cast<size_t>(1));
    const uint64_t up = traffic.at("node-b").bytes_pushed;
    // N cases sharing a 200 kB input must not cost N x 200 kB.
    Expect(up == one_tree, "uploaded " + std::to_string(up) + " bytes, a deduplicated sweep needs " +
                               std::to_string(one_tree));
}

TEST(PlacerRerunWithOneChangedInputMovesOnlyThatCase) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    constexpr int kCases = 4;
    TempDir src;
    zeromesh::testing::WriteFile(src / "shared.bin", Blob(150000, 's'));
    auto build = [&](int changed) {
        std::vector<v1::Digest> roots;
        for (int i = 0; i < kCases; ++i) {
            const std::string body = "case " + std::to_string(i) + (i == changed ? " CHANGED\n" : "\n");
            zeromesh::testing::WriteFile(src / "case.txt", body);
            roots.push_back(zeromesh::node::ToProto(a.merkle->BuildTree(src.path())));
        }
        return roots;
    };

    const std::string first = req.jm->Submit(Sweep(build(-1)));
    WaitJob(*req.jm, first);
    const uint64_t after_first = req.placer->Traffic().at("node-b").bytes_pushed;

    const std::string second = req.jm->Submit(Sweep(build(2)));
    WaitJob(*req.jm, second);
    auto st = req.jm->Get(second);
    int hits = 0, ran = 0;
    for (const auto& [name, r] : st->results()) {
        (r.cache_hit() ? hits : ran)++;
    }
    ASSERT_EQ(hits, kCases - 1);
    ASSERT_EQ(ran, 1);

    // Only the changed case.txt and the manifests above it travel again (shared.bin stays put).
    const uint64_t delta = req.placer->Traffic().at("node-b").bytes_pushed - after_first;
    Expect(delta < 4096, "re-run uploaded " + std::to_string(delta) + " bytes for one changed 10-byte input");
    Expect(delta > 0, "re-run uploaded nothing, but one input changed");
}
