#include "tests/unit/node_fixture.hpp"

#include "node/action_cache.hpp"
#include "node/convert.hpp"
#include "node/job_manager.hpp"
#include "sched/placer.hpp"

#include <chrono>
#include <memory>
#include <vector>

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
        placer = std::make_shared<zeromesh::sched::Placer>(self.executor, self.peers, factory, self.cas, self.merkle,
                                                           self.node_id, [&self] { return self.telemetry; });
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

    const std::string id = req.jm->Submit(RemoteOnly("sleep 60"));
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
