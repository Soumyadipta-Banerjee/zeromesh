#include "tests/unit/node_fixture.hpp"

#include "run/item_cache.hpp"

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

v1::BatchSpec Shell(const std::string& script) {
    return zeromesh::testing::ShBatch({{"t", script}});
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
    ASSERT_EQ(needed.size(), static_cast<size_t>(2)); // the manifest and in.txt

    v1::BatchSpec spec = Shell("cat in.txt; printf %s \"$ZM_NODE_ID\" > who");
    spec.mutable_items(0)->add_outputs("who");
    spec.set_stream_logs(true);
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

    std::vector<v1::BatchEvent> events;
    auto finished = client->Run("t1", [&](const v1::BatchEvent& ev) { events.push_back(ev); }, &err);
    Expect(finished.has_value(), err);
    Expect(finished->error().empty() && !finished->cancelled(), finished->error());
    std::optional<v1::ItemOutcome> outcome;
    bool saw_start = false, saw_log = false;
    for (const auto& ev : events) {
        saw_start = saw_start || ev.started() == "t";
        saw_log = saw_log || (ev.has_log() && ev.log().fd() == 1 && ev.log().chunk() == "hello from A\n");
        if (ev.has_done()) outcome = ev.done();
    }
    ASSERT_TRUE(saw_start && saw_log && outcome.has_value());
    Expect(outcome->failure() == v1::FAILURE_NONE, outcome->message());
    ASSERT_EQ(outcome->stdout_tail(), std::string("hello from A\n"));
    ASSERT_EQ(outcome->outputs_size(), 1);
    const v1::Digest who = outcome->outputs(0).digest();
    ASSERT_FALSE(a.cas->HasBlob(who.hash()));
    Expect(client->Pull(*a.cas, {who, outcome->stdout()}, &err), err);
    ASSERT_EQ(a.cas->GetBlob(who.hash()), std::optional<std::string>("node-b"));
    ASSERT_EQ(a.cas->GetBlob(outcome->stdout().hash()), std::optional<std::string>("hello from A\n"));

    // The reservation was used up.
    Expect(!client->Run("t1", nullptr, &err).has_value(), "second run of t1 succeeded");
}

TEST(NodeApiLargeOfferAccepts) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);

    v1::BatchSpec spec = Shell("true");
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

TEST(NodeApiOfferChecksTheEnvironment) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    auto client = a.ClientTo(b);
    std::string err;

    v1::BatchSpec missing = Shell("true");
    missing.mutable_env()->add_programs()->set_name("definitely-not-a-binary-xyz");
    auto reply = client->Offer("t1", missing, {}, &err);
    Expect(reply.has_value(), err);
    ASSERT_FALSE(reply->accepted());
    ASSERT_TRUE(reply->reject_kind() == v1::REJECT_ENV);
    Expect(reply->reason().find("is not installed") != std::string::npos, reply->reason());

    v1::BatchSpec other_build = Shell("true");
    auto* sh = other_build.mutable_env()->add_programs();
    sh->set_name("sh");
    sh->set_digest(std::string(64, 'e'));
    reply = client->Offer("t2", other_build, {}, &err);
    ASSERT_TRUE(reply && !reply->accepted() && reply->reject_kind() == v1::REJECT_TOOLCHAIN);

    // A version constraint is checked with the donor's probe.
    b.service->SetVersionProbe([](const v1::Program&, const std::string&) { return std::optional<std::string>("tool 3.9.1"); });
    v1::BatchSpec old = Shell("true");
    auto* p = old.mutable_env()->add_programs();
    p->set_name("sh");
    p->set_version(">=4.0");
    reply = client->Offer("t3", old, {}, &err);
    ASSERT_TRUE(reply && !reply->accepted() && reply->reject_kind() == v1::REJECT_ENV);
    Expect(reply->reason().find("3.9.1 does not satisfy >=4.0") != std::string::npos, reply->reason());
    p->set_version(">=3.9,<4");
    reply = client->Offer("t4", old, {}, &err);
    Expect(reply && reply->accepted(), reply ? reply->reason() : err);

    // A shared path the donor does not have.
    v1::BatchSpec shared = Shell("true");
    shared.mutable_env()->add_shared_paths("/definitely/not/mounted/here");
    reply = client->Offer("t5", shared, {}, &err);
    ASSERT_TRUE(reply && !reply->accepted() && reply->reject_kind() == v1::REJECT_ENV);
}

TEST(NodeApiFairShareMakesRoomForAWaitingRequester) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull donor("node-d", key, 4), r1("node-r1", key), r2("node-r2", key);
    donor.telemetry.cores_total = 5; // donates 4, and runs 4 at once
    donor.Start();
    r1.Start();
    r2.Start();
    r1.Admit(donor);
    r2.Admit(donor);
    auto c1 = r1.ClientTo(donor);
    auto c2 = r2.ClientTo(donor);
    std::string err;
    // r1 alone may take everything.
    for (int i = 0; i < 4; ++i) {
        auto reply = c1->Offer("a" + std::to_string(i), Shell("true"), {}, &err);
        Expect(reply && reply->accepted(), reply ? reply->reason() : err);
    }
    // r2 finds the donor full and waits.
    auto full = c2->Offer("b0", Shell("true"), {}, &err);
    ASSERT_TRUE(full && !full->accepted() && full->reject_kind() == v1::REJECT_BUSY);
    // r1 releases its reservations; from now on it may hold only its share (2 of 4).
    for (int i = 0; i < 4; ++i) {
        v1::BatchRef ref;
        ref.set_batch_id("a" + std::to_string(i));
        // CancelBatch on an offered batch drops its reservation.
        grpc::ClientContext ctx;
        v1::Empty empty;
        (void)RawStub(r1, donor)->CancelBatch(&ctx, ref, &empty);
    }
    for (int i = 0; i < 2; ++i) {
        auto reply = c1->Offer("c" + std::to_string(i), Shell("true"), {}, &err);
        Expect(reply && reply->accepted(), reply ? reply->reason() : err);
    }
    auto over = c1->Offer("c2", Shell("true"), {}, &err);
    ASSERT_TRUE(over && !over->accepted() && over->reject_kind() == v1::REJECT_BUSY);
    Expect(over->reason().find("fair share") != std::string::npos, over->reason());
    auto mine = c2->Offer("b1", Shell("true"), {}, &err);
    Expect(mine && mine->accepted(), mine ? mine->reason() : err);
}

TEST(NodeApiTakesWorkOnlyThroughTheExecutorsHold) {
    // The executor is the one place a node takes work: an offer is held there, beside the node's
    // own batches, and given back when the requester cancels it.
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull donor("node-d", key), r("node-r", key);
    donor.Start();
    r.Start();
    r.Admit(donor);
    auto c = r.ClientTo(donor);
    std::string err;
    // The donor's own batches hold both of its slots: no room for anyone else's.
    ASSERT_TRUE(donor.executor->Hold("node-d/own1", Shell("true")));
    ASSERT_TRUE(donor.executor->Hold("node-d/own2", Shell("true")));
    auto full = c->Offer("a0", Shell("true"), {}, &err);
    ASSERT_TRUE(full && !full->accepted() && full->reject_kind() == v1::REJECT_BUSY);
    donor.executor->Release("node-d/own1");
    auto taken = c->Offer("a1", Shell("true"), {}, &err);
    Expect(taken && taken->accepted(), taken ? taken->reason() : err);
    ASSERT_EQ(donor.executor->CoresReserved(), 2); // its own batch and the offer
    // While the offer holds the slot, the donor's own placer cannot take it.
    ASSERT_FALSE(donor.executor->Hold("node-d/own3", Shell("true")));
    v1::BatchRef ref;
    ref.set_batch_id("a1");
    grpc::ClientContext ctx;
    v1::Empty empty;
    ASSERT_TRUE(RawStub(r, donor)->CancelBatch(&ctx, ref, &empty).ok());
    ASSERT_EQ(donor.executor->CoresReserved(), 1);
    donor.executor->Release("node-d/own2");

    // A requester that goes away between Offer and RunBatch: its offer expires and gives the room back.
    auto gone = c->Offer("a2", Shell("true"), {}, &err);
    Expect(gone && gone->accepted(), gone ? gone->reason() : err);
    ASSERT_EQ(donor.executor->CoresReserved(), 1);
    donor.service->ExpireOffers(std::chrono::steady_clock::now() + zeromesh::node::kOfferReservation);
    ASSERT_EQ(donor.executor->CoresReserved(), 0);
}

TEST(NodeApiAnswersTeamCacheLookups) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    auto cache = std::make_shared<zeromesh::run::ItemCache>(b.cas, b.merkle);
    b.service->SetItemCache(cache);
    const auto out = b.cas->PutBlob("object");
    v1::CachedItem item;
    auto* f = item.add_outputs();
    f->set_path("a.o");
    *f->mutable_digest() = zeromesh::node::ToProto(out);
    item.set_node("node-b");
    const std::string k(64, '1'), weak(64, '2');
    cache->Put(k, item);
    v1::Footprint fp;
    auto* ff = fp.add_files();
    ff->set_path("src/a.c");
    ff->set_hash("f:abc");
    cache->AddFootprint(weak, fp);

    v1::CacheLookup req;
    req.add_keys(k);
    req.add_keys(std::string(64, '3')); // unknown
    req.add_weaks(weak);
    std::string err;
    auto reply = a.ClientTo(b)->LookupCache(req, std::chrono::seconds(5), &err);
    Expect(reply.has_value(), err);
    ASSERT_EQ(reply->hits_size(), 1);
    ASSERT_EQ(reply->hits(0).key(), k);
    ASSERT_EQ(reply->footprints_size(), 1);
    ASSERT_EQ(reply->footprints(0).footprints(0).files(0).path(), std::string("src/a.c"));

    v1::CacheForget forget;
    forget.add_keys(k);
    Expect(a.ClientTo(b)->ForgetCache(forget, &err), err);
    ASSERT_FALSE(cache->Get(k).has_value());
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
    ASSERT_EQ(PushRaw(*stub, {MakeChunk(good.hash, 2, "hello", true)}).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_EQ(PushRaw(*stub, {MakeChunk("../../etc/passwd", 5, "hello", true)}).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_EQ(PushRaw(*stub, {MakeChunk("", 0, "hello", true)}).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_EQ(PushRaw(*stub, {MakeChunk(good.hash, good.size_bytes, "hel", false)}).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    ASSERT_FALSE(b.cas->HasBlob(good.hash));

    // Correct bytes in two chunks are stored.
    ASSERT_TRUE(
        PushRaw(*stub, {MakeChunk(good.hash, good.size_bytes, "he", false), MakeChunk("", 0, "llo", true)}).ok());
    ASSERT_EQ(b.cas->GetBlob(good.hash), std::optional<std::string>("hello"));

    // Unadmitted callers are refused by every remote-execution RPC.
    auto rogue = RawStub(outsider, b);
    ASSERT_EQ(PushRaw(*rogue, {MakeChunk(bad.hash, bad.size_bytes, "jello", true)}).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);
    ASSERT_FALSE(b.cas->HasBlob(bad.hash));
    {
        grpc::ClientContext ctx;
        v1::Offer offer;
        offer.set_batch_id("x");
        *offer.mutable_spec() = Shell("true");
        v1::OfferReply reply;
        ASSERT_EQ(rogue->OfferBatch(&ctx, offer, &reply).error_code(), grpc::StatusCode::PERMISSION_DENIED);
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
    std::optional<v1::BatchFinished> result;
    std::string run_err;
    std::thread runner([&] {
        result = client->Run(
            "t1",
            [&](const v1::BatchEvent& ev) {
                if (ev.has_started()) running = true;
            },
            &run_err);
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
    ASSERT_TRUE(result->cancelled());
    ASSERT_TRUE(took < std::chrono::seconds(8)); // SIGTERM, then SIGKILL after the grace period
}
