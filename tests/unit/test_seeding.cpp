#include "tests/unit/node_fixture.hpp"

#include "node/action_cache.hpp"
#include "node/convert.hpp"
#include "node/job_manager.hpp"
#include "sched/placer.hpp"
#include "sched/seed_gate.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace v1 = zeromesh::v1;
using zeromesh::sched::SeedGate;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;
using namespace std::chrono_literals;

namespace {

void SExpect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

struct SeedRequester {
    std::shared_ptr<zeromesh::sched::Placer> placer;
    std::unique_ptr<zeromesh::node::JobManager> jm;

    explicit SeedRequester(TestNodeFull& self) {
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

v1::JobSpec OneTask(const v1::Digest& root) {
    v1::JobSpec job;
    job.set_name("seed");
    auto* t = job.add_tasks();
    t->set_name("t");
    t->add_argv("sh");
    t->add_argv("-c");
    t->add_argv("test -s big.bin && echo ok");
    *t->mutable_input_root() = root;
    t->mutable_policy()->set_may_run_local(false);
    return job;
}

// Waits for the job and returns who ran it; throws if it failed.
std::string RunAndWait(SeedRequester& req, const v1::JobSpec& job) {
    const std::string id = req.jm->Submit(job);
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!req.jm->Get(id)->done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
    auto st = req.jm->Get(id);
    SExpect(st->done(), "job did not finish");
    SExpect(st->tasks().at("t") == v1::SUCCEEDED, st->results().at("t").error());
    return st->results().at("t").executed_by();
}

// Lets `node` reach its peers by id, as the daemon's client_for_node does.
void LetReach(TestNodeFull& node) {
    TestNodeFull* self = &node;
    node.service->SetPeerClientFactory([self](const std::string& id) -> std::shared_ptr<zeromesh::node::NodeClient> {
        auto peer = self->peers->Get(id);
        if (!peer) return nullptr;
        return std::make_shared<zeromesh::node::NodeClient>(peer->ip_address + ":" + std::to_string(peer->node_port),
                                                            self->id, self->pins, peer->id);
    });
}

constexpr size_t kBig = 200000;

v1::Digest TreeWithBigFile(TestNodeFull& a, const std::filesystem::path& dir, const std::string& extra) {
    zeromesh::testing::WriteFile(dir / "big.bin", std::string(kBig, 'B'));
    zeromesh::testing::WriteFile(dir / "extra.txt", extra);
    return zeromesh::node::ToProto(a.merkle->BuildTree(dir));
}

} // namespace

TEST(SeedGateCapsConcurrentSendsPerSource) {
    SeedGate gate(/*per_source=*/3, /*lan_wide=*/100);
    std::atomic<int> now{0}, peak{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 12; ++i) {
        threads.emplace_back([&] {
            auto token = gate.Acquire("node-b", 10s);
            if (!token) return;
            int n = ++now;
            int p = peak.load();
            while (n > p && !peak.compare_exchange_weak(p, n)) {
            }
            std::this_thread::sleep_for(40ms);
            --now;
        });
    }
    for (auto& t : threads) t.join();
    ASSERT_EQ(peak.load(), 3);
    ASSERT_EQ(gate.ActiveTotal(), 0);
}

TEST(SeedGateLanCapBoundsAllSourcesTogether) {
    SeedGate gate(/*per_source=*/3, /*lan_wide=*/2);
    std::atomic<int> now{0}, peak{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 9; ++i) {
        threads.emplace_back([&, i] {
            auto token = gate.Acquire(i % 3 == 0 ? "node-b" : (i % 3 == 1 ? "node-c" : "node-d"), 10s);
            if (!token) return;
            int n = ++now;
            int p = peak.load();
            while (n > p && !peak.compare_exchange_weak(p, n)) {
            }
            std::this_thread::sleep_for(40ms);
            --now;
        });
    }
    for (auto& t : threads) t.join();
    ASSERT_EQ(peak.load(), 2); // default LAN-wide cap: shared Wi-Fi airtime
}

TEST(SeedGateDefaultLanCapIsTwo) {
    SeedGate gate;
    auto a = gate.Acquire("x", 0ms);
    auto b = gate.Acquire("y", 0ms);
    ASSERT_TRUE(a.has_value() && b.has_value());
    ASSERT_TRUE(!gate.Acquire("z", 0ms).has_value());
    ASSERT_EQ(gate.Active("x"), 1);
    a.reset();
    ASSERT_TRUE(gate.Acquire("z", 0ms).has_value());
}

TEST(SeedGateGivesUpAfterTimeout) {
    SeedGate gate(1, 1);
    auto held = gate.Acquire("node-b", 0ms);
    ASSERT_TRUE(held.has_value());
    const auto t0 = std::chrono::steady_clock::now();
    ASSERT_TRUE(!gate.Acquire("node-b", 150ms).has_value());
    ASSERT_TRUE(std::chrono::steady_clock::now() - t0 >= 140ms);
}

TEST(NodeClientSeedBlobsMakesDonorFetchFromPeer) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    c.Admit(b);
    c.peers->Upsert(b.AsPeer());
    LetReach(c);

    TempDir src;
    zeromesh::testing::WriteFile(src / "in.bin", std::string(50000, 'x'));
    auto root = a.merkle->BuildTree(src.path());
    std::vector<v1::Digest> digests;
    for (const auto& d : a.merkle->CollectTree(root)) digests.push_back(zeromesh::node::ToProto(d));
    std::string err;
    SExpect(a.ClientTo(b)->Push(*a.cas, digests, &err), err); // only b holds the tree

    std::vector<v1::Digest> fetched;
    SExpect(a.ClientTo(c)->SeedBlobs("node-b", digests, &fetched, &err), err);
    ASSERT_EQ(fetched.size(), digests.size());
    for (const auto& d : digests) ASSERT_TRUE(c.cas->HasBlob(d.hash()));
}

TEST(NodeClientSeedBlobsReportsWhatItCouldNotFetch) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), c("node-c", key);
    a.Start();
    c.Start();
    a.Admit(c);
    LetReach(c); // c knows nobody

    TempDir src;
    zeromesh::testing::WriteFile(src / "in.bin", std::string(1000, 'y'));
    std::vector<v1::Digest> digests;
    for (const auto& d : a.merkle->CollectTree(a.merkle->BuildTree(src.path()))) {
        digests.push_back(zeromesh::node::ToProto(d));
    }
    std::vector<v1::Digest> fetched;
    std::string err;
    ASSERT_TRUE(!a.ClientTo(c)->SeedBlobs("node-ghost", digests, &fetched, &err));
    ASSERT_EQ(fetched.size(), static_cast<size_t>(0));
    ASSERT_TRUE(!err.empty());
}

TEST(SeedingDonorFetchesSharedInputFromPeerNotRequester) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    c.Admit(b);
    c.peers->Upsert(b.AsPeer());
    LetReach(c);
    a.peers->Upsert(b.AsPeer()); // only b is known at first, so job 1 must run there
    SeedRequester req(a);

    TempDir src;
    const v1::Digest root1 = TreeWithBigFile(a, src.path(), "one\n");
    ASSERT_EQ(RunAndWait(req, OneTask(root1)), std::string("node-b"));
    const auto after_first = req.placer->Traffic();
    SExpect(after_first.at("node-b").bytes_pushed >= kBig, "first job should upload the big input once");

    // Job 2 shares big.bin but differs in one small file. c joins and b is busy, so c runs it.
    auto loaded_b = b.AsPeer();
    loaded_b.cpu_load_pct = 90;
    a.peers->Upsert(loaded_b);
    auto idle_c = c.AsPeer();
    idle_c.cpu_load_pct = 1;
    a.peers->Upsert(idle_c);
    const v1::Digest root2 = TreeWithBigFile(a, src.path(), "two, different\n");
    ASSERT_EQ(RunAndWait(req, OneTask(root2)), std::string("node-c"));

    const auto t = req.placer->Traffic();
    // The requester sent c only the small changed file and the manifests above it.
    SExpect(t.at("node-c").bytes_pushed < 4096, "pushed to c: " + std::to_string(t.at("node-c").bytes_pushed));
    // c took the big blob from b.
    SExpect(t.at("node-c").bytes_seeded >= kBig, "seeded to c: " + std::to_string(t.at("node-c").bytes_seeded));
    // Requester upload over both jobs: one copy of big.bin plus small files.
    const uint64_t total = t.at("node-b").bytes_pushed + t.at("node-c").bytes_pushed;
    SExpect(total < kBig + kBig / 2, "requester uploaded " + std::to_string(total));
}

TEST(SeedingFallsBackToRequesterWhenPeerCannotBeReached) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    // c has no route to b: its factory finds no peer.
    c.service->SetPeerClientFactory([](const std::string&) -> std::shared_ptr<zeromesh::node::NodeClient> {
        return nullptr;
    });
    a.peers->Upsert(b.AsPeer());
    SeedRequester req(a);

    TempDir src;
    ASSERT_EQ(RunAndWait(req, OneTask(TreeWithBigFile(a, src.path(), "one\n"))), std::string("node-b"));

    auto loaded_b = b.AsPeer();
    loaded_b.cpu_load_pct = 90;
    a.peers->Upsert(loaded_b);
    auto idle_c = c.AsPeer();
    idle_c.cpu_load_pct = 1;
    a.peers->Upsert(idle_c);
    ASSERT_EQ(RunAndWait(req, OneTask(TreeWithBigFile(a, src.path(), "two, different\n"))), std::string("node-c"));

    const auto t = req.placer->Traffic();
    SExpect(t.at("node-c").bytes_pushed >= kBig, "requester must push the big blob itself: " +
                                                     std::to_string(t.at("node-c").bytes_pushed));
    ASSERT_EQ(t.at("node-c").bytes_seeded, static_cast<uint64_t>(0));
}

TEST(SeedingCanBeDisabled) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    c.Admit(b);
    c.peers->Upsert(b.AsPeer());
    LetReach(c);
    a.peers->Upsert(b.AsPeer());
    SeedRequester req(a);
    zeromesh::sched::Placer::SeedConfig off;
    off.enabled = false;
    req.placer->SetSeeding(off);

    TempDir src;
    ASSERT_EQ(RunAndWait(req, OneTask(TreeWithBigFile(a, src.path(), "one\n"))), std::string("node-b"));
    auto loaded_b = b.AsPeer();
    loaded_b.cpu_load_pct = 90;
    a.peers->Upsert(loaded_b);
    auto idle_c = c.AsPeer();
    idle_c.cpu_load_pct = 1;
    a.peers->Upsert(idle_c);
    ASSERT_EQ(RunAndWait(req, OneTask(TreeWithBigFile(a, src.path(), "two, different\n"))), std::string("node-c"));
    const auto t = req.placer->Traffic();
    SExpect(t.at("node-c").bytes_pushed >= kBig, "no seeding: requester pushes");
    ASSERT_EQ(t.at("node-c").bytes_seeded, static_cast<uint64_t>(0));
}
