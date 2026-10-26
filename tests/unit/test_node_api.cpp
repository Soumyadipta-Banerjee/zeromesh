#include "tests/unit/node_fixture.hpp"

#include "node/convert.hpp"
#include "node/node_client.hpp"
#include "store/merkle.hpp"

#include <grpcpp/client_context.h>

#include <atomic>
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>

namespace v1 = zeromesh::v1;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;

namespace {

void Expect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

std::vector<v1::Digest> ToProto(const std::vector<zeromesh::store::Digest>& in) {
    std::vector<v1::Digest> out;
    for (const auto& d : in) out.push_back(zeromesh::node::ToProto(d));
    return out;
}

v1::TaskSpec Shell(const std::string& script) {
    v1::TaskSpec s;
    s.set_name("t");
    s.add_argv("sh");
    s.add_argv("-c");
    s.add_argv(script);
    return s;
}

// A raw stub to `peer` that accepts any server certificate, so only the server's gate decides.
std::unique_ptr<v1::NodeApi::Stub> RawStub(const TestNodeFull& from, const TestNodeFull& peer) {
    return v1::NodeApi::NewStub(zeromesh::net::PeerChannel(
        peer.address(), zeromesh::net::ClientCreds(from.id, [](std::string_view) { return true; })));
}

grpc::Status PushRaw(v1::NodeApi::Stub& stub, const std::vector<v1::Chunk>& chunks) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    v1::Empty empty;
    auto w = stub.PushBlobs(&ctx, &empty);
    for (const auto& c : chunks) {
        if (!w->Write(c)) break;
    }
    w->WritesDone();
    return w->Finish();
}

v1::Chunk MakeChunk(const std::string& hash, int64_t size, const std::string& data, bool last) {
    v1::Chunk c;
    if (!hash.empty()) {
        c.mutable_digest()->set_hash(hash);
        c.mutable_digest()->set_size_bytes(size);
    }
    c.set_data(data);
    c.set_last(last);
    return c;
}

} // namespace

TEST(NodeApiRemoteRun) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);

    TempDir src;
    zeromesh::testing::WriteFile(src / "in.txt", "hello from A\n");
    auto root = a.merkle->BuildTree(src.path());
    auto needed = ToProto(a.merkle->CollectTree(root));
    ASSERT_EQ(needed.size(), static_cast<size_t>(2));  // the manifest and in.txt

    v1::TaskSpec spec = Shell("cat in.txt; printf %s \"$ZM_NODE_ID\" > who");
    spec.add_outputs("who");
    *spec.mutable_input_root() = zeromesh::node::ToProto(root);

    auto client = a.ClientTo(b);
    std::string err;
    // Running without an accepted offer is refused.
    Expect(!client->Run("t1", nullptr, &err).has_value() && err.find("no reservation") != std::string::npos, err);

    auto reply = client->Offer("t1", spec, needed, &err);
    Expect(reply.has_value(), err);
    Expect(reply->accepted(), reply->reason());
    ASSERT_EQ(static_cast<size_t>(reply->missing_size()), needed.size());
    // Missing inputs: the run is refused until they are pushed.
    Expect(!client->Run("t1", nullptr, &err).has_value() && err.find("not pushed") != std::string::npos, err);

    std::vector<v1::Digest> missing(reply->missing().begin(), reply->missing().end());
    Expect(client->Push(*a.cas, missing, &err), err);
    ASSERT_TRUE(b.cas->HasBlob(root.hash));

    std::vector<v1::JobEvent> events;
    auto result = client->Run("t1", [&](const v1::JobEvent& ev) { events.push_back(ev); }, &err);
    Expect(result.has_value(), err);
    Expect(result->exit_code() == 0, "exit " + std::to_string(result->exit_code()) + ": " + result->error());
    ASSERT_EQ(result->stdout_head(), std::string("hello from A\n"));
    ASSERT_EQ(result->executed_by(), std::string("node-b"));
    ASSERT_TRUE(!events.empty() && events.back().has_result() && events.back().state() == v1::SUCCEEDED);
    bool saw_running = false, saw_log = false;
    for (const auto& ev : events) {
        saw_running = saw_running || ev.state() == v1::RUNNING;
        saw_log = saw_log || (ev.log_fd() == 1 && ev.log_chunk() == "hello from A\n");
    }
    ASSERT_TRUE(saw_running && saw_log);

    ASSERT_EQ(result->outputs_size(), 1);
    const v1::Digest who = result->outputs(0).digest();
    ASSERT_FALSE(a.cas->HasBlob(who.hash()));
    Expect(client->Pull(*a.cas, {who, result->stdout()}, &err), err);
    ASSERT_EQ(a.cas->GetBlob(who.hash()), std::optional<std::string>("node-b"));
    ASSERT_EQ(a.cas->GetBlob(result->stdout().hash()), std::optional<std::string>("hello from A\n"));

    // The reservation was used up.
    Expect(!client->Run("t1", nullptr, &err).has_value(), "second run of t1 succeeded");
}

TEST(NodeApiLargeOfferAccepts) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);

    v1::TaskSpec spec = Shell("true");
    std::vector<v1::Digest> needed;
    needed.reserve(70000);
    for (int i = 0; i < 70000; ++i) {
        v1::Digest d;
        char hex[65];
        std::snprintf(hex, sizeof(hex), "%064x", i);
        d.set_hash(hex);
        d.set_size_bytes(1);
        needed.push_back(d);
    }
    ASSERT_TRUE(zeromesh::node::EncodedOfferByteSize("big-offer", spec, needed) <
                static_cast<size_t>(zeromesh::node::kNodeApiMaxMessageSize));

    std::string err;
    auto reply = a.ClientTo(b)->Offer("big-offer", spec, needed, &err);
    Expect(reply.has_value(), err);
    ASSERT_TRUE(reply->accepted());
}

TEST(NodeApiOfferRejectsMissingCommand) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    v1::TaskSpec spec;
    spec.add_argv("definitely-not-a-binary-xyz");
    std::string err;
    auto reply = a.ClientTo(b)->Offer("t1", spec, {}, &err);
    Expect(reply.has_value(), err);
    ASSERT_FALSE(reply->accepted());
    Expect(reply->reason().find("not found on this node") != std::string::npos, reply->reason());
}

TEST(NodeApiBlobDigestVerified) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), outsider("node-c", key);
    a.Start();
    b.Start();
    a.Admit(b);

    const auto good = zeromesh::store::CasStore::ComputeDigest("hello");
    const auto bad = zeromesh::store::CasStore::ComputeDigest("jello");
    auto stub = RawStub(a, b);

    // Declared digest of "hello", bytes "jello".
    ASSERT_EQ(PushRaw(*stub, {MakeChunk(good.hash, good.size_bytes, "jello", true)}).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_FALSE(b.cas->HasBlob(good.hash));
    ASSERT_FALSE(b.cas->HasBlob(bad.hash));
    // More bytes than declared, a hash that is not one, a chunk without digest, a truncated object.
    ASSERT_EQ(PushRaw(*stub, {MakeChunk(good.hash, 2, "hello", true)}).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_EQ(PushRaw(*stub, {MakeChunk("../../etc/passwd", 5, "hello", true)}).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_EQ(PushRaw(*stub, {MakeChunk("", 0, "hello", true)}).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_EQ(PushRaw(*stub, {MakeChunk(good.hash, good.size_bytes, "hel", false)}).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_FALSE(b.cas->HasBlob(good.hash));

    // Correct bytes in two chunks are stored.
    ASSERT_TRUE(PushRaw(*stub, {MakeChunk(good.hash, good.size_bytes, "he", false), MakeChunk("", 0, "llo", true)}).ok());
    ASSERT_EQ(b.cas->GetBlob(good.hash), std::optional<std::string>("hello"));

    // Unadmitted callers are refused by every remote-execution RPC.
    auto rogue = RawStub(outsider, b);
    ASSERT_EQ(PushRaw(*rogue, {MakeChunk(bad.hash, bad.size_bytes, "jello", true)}).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);
    ASSERT_FALSE(b.cas->HasBlob(bad.hash));
    {
        grpc::ClientContext ctx;
        v1::Offer offer;
        offer.set_task_id("x");
        offer.mutable_spec()->add_argv("sh");
        v1::OfferReply reply;
        ASSERT_EQ(rogue->OfferTask(&ctx, offer, &reply).error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }
    {
        grpc::ClientContext ctx;
        v1::Digests req;
        *req.add_digests() = zeromesh::node::ToProto(good);
        auto reader = rogue->PullBlobs(&ctx, req);
        v1::Chunk c;
        ASSERT_FALSE(reader->Read(&c));
        ASSERT_EQ(reader->Finish().error_code(), grpc::StatusCode::PERMISSION_DENIED);
    }
}

TEST(NodeApiCancel) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    auto client = a.ClientTo(b);
    std::string err;
    auto reply = client->Offer("t1", Shell("sleep 30"), {}, &err);
    Expect(reply && reply->accepted(), err + (reply ? reply->reason() : ""));

    std::atomic<bool> running{false};
    std::optional<v1::TaskResult> result;
    std::string run_err;
    std::thread runner([&] {
        result = client->Run("t1", [&](const v1::JobEvent& ev) {
            if (ev.state() == v1::RUNNING) running = true;
        }, &run_err);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto t0 = std::chrono::steady_clock::now();
    client->Cancel("t1");
    runner.join();
    const auto took = std::chrono::steady_clock::now() - t0;
    Expect(result.has_value(), run_err);
    ASSERT_TRUE(result->cancelled() || !result->error().empty());
    ASSERT_TRUE(took < std::chrono::seconds(2));
}
