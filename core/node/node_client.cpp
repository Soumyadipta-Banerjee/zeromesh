#include "node/node_client.hpp"

#include "net/tls.hpp"
#include "node/admission.hpp"
#include "node/blob_transfer.hpp"
#include "node/convert.hpp"

#include <grpcpp/client_context.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace zeromesh::node {

namespace {

void SetError(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
}

std::string Describe(const char* what, const grpc::Status& st) {
    return std::string(what) + " failed (code " + std::to_string(static_cast<int>(st.error_code())) +
           "): " + st.error_message();
}

// 10 s plus 1 s per MiB: a transfer slower than 1 MiB/s counts as stalled.
void SetTransferDeadline(grpc::ClientContext& ctx, const std::vector<v1::Digest>& digests) {
    int64_t bytes = 0;
    for (const auto& d : digests) bytes += std::max<int64_t>(d.size_bytes(), 0);
    ctx.set_deadline(std::chrono::system_clock::now() + kNodeRpcTimeout + std::chrono::seconds(bytes >> 20));
}

// For one admission: accepts the first server fingerprint seen and afterwards only
// that one, so Admit and AdmitConfirm provably reach the same certificate.
struct FirstSeenFingerprint {
    std::mutex mu;
    std::string fp;

    bool Accept(std::string_view seen) {
        std::lock_guard lock(mu);
        if (fp.empty()) fp = std::string(seen);
        return fp == seen;
    }
    std::string Get() {
        std::lock_guard lock(mu);
        return fp;
    }
};

} // namespace

NodeClient::NodeClient(std::string address, net::Identity self, std::shared_ptr<net::PinStore> pins,
                       std::optional<std::string> expected_node_id)
    : address_(std::move(address)), self_(std::move(self)), pins_(std::move(pins)),
      peer_node_id_(std::move(expected_node_id)) {
    if (!pins_) throw std::invalid_argument("NodeClient: missing pin store");
}

std::optional<std::string> NodeClient::peer_node_id() const {
    std::lock_guard lock(mu_);
    return peer_node_id_;
}

void NodeClient::SetDeadline(grpc::ClientContext& ctx) {
    ctx.set_deadline(std::chrono::system_clock::now() + kNodeRpcTimeout);
}

bool NodeClient::Admit(const net::TeamKey& key, std::string* error) {
    auto seen = std::make_shared<FirstSeenFingerprint>();
    auto channel = net::PeerChannel(
        address_, net::ClientCreds(self_, [seen](std::string_view fp) { return seen->Accept(fp); }));
    auto stub = v1::NodeApi::NewStub(channel);

    const std::string nonce_c = AdmitNonce();
    v1::AdmitHello hello;
    hello.set_node_id(self_.node_id);
    hello.set_nonce(nonce_c);
    v1::AdmitChallenge challenge;
    {
        grpc::ClientContext ctx;
        SetDeadline(ctx);
        grpc::Status st = stub->Admit(&ctx, hello, &challenge);
        if (!st.ok()) return SetError(error, Describe("Admit", st)), false;
    }

    const std::string fp_server = seen->Get();
    if (!net::IsFingerprint(fp_server)) return SetError(error, "no server certificate seen"), false;
    if (!net::IsValidNodeId(challenge.node_id()) || challenge.nonce().size() != kAdmitNonceBytes) {
        return SetError(error, "malformed admission challenge"), false;
    }
    if (!AdmitMacMatches(challenge.mac(), AdmitServerMac(key, nonce_c, fp_server, self_.fingerprint))) {
        return SetError(error, "peer failed the team proof (different team?)"), false;
    }
    if (auto expected = peer_node_id(); expected && *expected != challenge.node_id()) {
        return SetError(error, "peer claims node id " + challenge.node_id() + ", expected " + *expected), false;
    }

    v1::AdmitProof proof;
    const auto mac_c = AdmitClientMac(key, challenge.nonce(), self_.fingerprint, fp_server);
    proof.set_mac(std::string(mac_c.begin(), mac_c.end()));
    v1::Empty empty;
    {
        grpc::ClientContext ctx;
        SetDeadline(ctx);
        grpc::Status st = stub->AdmitConfirm(&ctx, proof, &empty);
        if (!st.ok()) return SetError(error, Describe("AdmitConfirm", st)), false;
    }

    try {
        pins_->Pin(challenge.node_id(), fp_server);
    } catch (const std::exception& e) {
        return SetError(error, std::string("cannot pin peer: ") + e.what()), false;
    }
    std::lock_guard lock(mu_);
    peer_node_id_ = challenge.node_id();
    pinned_stub_.reset();
    return true;
}

std::shared_ptr<v1::NodeApi::Stub> NodeClient::PinnedStub(std::string* error) {
    std::lock_guard lock(mu_);
    if (!peer_node_id_) return SetError(error, "peer not admitted (node id unknown)"), nullptr;
    if (!pins_->FingerprintFor(*peer_node_id_)) {
        return SetError(error, "peer " + *peer_node_id_ + " is not pinned"), nullptr;
    }
    if (!pinned_stub_) {
        // The pin is looked up at every handshake, so re-admission takes effect at once.
        auto accept = [pins = pins_, node = *peer_node_id_](std::string_view fp) {
            auto pinned = pins->FingerprintFor(node);
            return pinned && *pinned == fp;
        };
        pinned_stub_ =
            v1::NodeApi::NewStub(net::PeerChannel(address_, net::ClientCreds(self_, accept), /*keepalive=*/true));
    }
    return pinned_stub_;
}

std::optional<v1::PeerInfo> NodeClient::Ping(std::string* error) {
    auto stub = PinnedStub(error);
    if (!stub) return std::nullopt;
    grpc::ClientContext ctx;
    SetDeadline(ctx);
    v1::PeerInfo info;
    grpc::Status st = stub->Ping(&ctx, v1::Empty{}, &info);
    if (!st.ok()) return SetError(error, Describe("Ping", st)), std::nullopt;
    return info;
}

std::optional<v1::OfferReply> NodeClient::Offer(const std::string& task_id, const v1::TaskSpec& spec,
                                                const std::vector<v1::Digest>& needed, std::string* error) {
    auto stub = PinnedStub(error);
    if (!stub) return std::nullopt;
    v1::Offer req;
    req.set_task_id(task_id);
    *req.mutable_spec() = spec;
    for (const auto& d : needed) *req.add_needed() = d;
    grpc::ClientContext ctx;
    SetDeadline(ctx);
    v1::OfferReply reply;
    grpc::Status st = stub->OfferTask(&ctx, req, &reply);
    if (!st.ok()) return SetError(error, Describe("OfferTask", st)), std::nullopt;
    return reply;
}

bool NodeClient::Push(const store::CasStore& cas, const std::vector<v1::Digest>& digests, std::string* error) {
    if (digests.empty()) return true;
    auto stub = PinnedStub(error);
    if (!stub) return false;
    grpc::ClientContext ctx;
    SetTransferDeadline(ctx, digests);
    v1::Empty empty;
    auto writer = stub->PushBlobs(&ctx, &empty);
    std::string local_err;
    bool transport_failed = false;
    auto write = [&](const v1::Chunk& c) {
        if (writer->Write(c)) return true;
        transport_failed = true;
        return false;
    };
    for (const auto& d : digests) {
        local_err = SendBlob(cas, FromProto(d), write);
        if (!local_err.empty()) break;
    }
    if (!transport_failed) {
        if (!local_err.empty()) ctx.TryCancel();  // a local problem (e.g. blob missing): abort
        writer->WritesDone();
    }
    grpc::Status st = writer->Finish();
    if (!local_err.empty() && !transport_failed) return SetError(error, "PushBlobs: " + local_err), false;
    if (!st.ok()) return SetError(error, Describe("PushBlobs", st)), false;
    if (!local_err.empty()) return SetError(error, "PushBlobs: " + local_err), false;
    return true;
}

bool NodeClient::Pull(store::CasStore& cas, const std::vector<v1::Digest>& digests, std::string* error) {
    if (digests.empty()) return true;
    auto stub = PinnedStub(error);
    if (!stub) return false;
    v1::Digests req;
    std::unordered_set<std::string> wanted;
    for (const auto& d : digests) {
        if (!store::CasStore::IsValidHash(d.hash())) return SetError(error, "Pull: invalid digest"), false;
        *req.add_digests() = d;
        wanted.insert(d.hash());
    }
    grpc::ClientContext ctx;
    SetTransferDeadline(ctx, digests);
    auto reader = stub->PullBlobs(&ctx, req);
    BlobReceiver receiver(cas);
    std::string local_err;
    v1::Chunk chunk;
    while (reader->Read(&chunk)) {
        if (chunk.has_digest() && !wanted.contains(chunk.digest().hash())) {
            local_err = "peer sent blob " + chunk.digest().hash() + ", which was not requested";
        } else {
            local_err = receiver.Feed(chunk);
        }
        if (!local_err.empty()) {
            ctx.TryCancel();
            break;
        }
    }
    grpc::Status st = reader->Finish();
    if (!local_err.empty()) return SetError(error, "PullBlobs: " + local_err), false;
    if (!st.ok()) return SetError(error, Describe("PullBlobs", st)), false;
    if (std::string err = receiver.Finish(); !err.empty()) return SetError(error, "PullBlobs: " + err), false;
    for (const auto& d : receiver.received()) wanted.erase(d.hash);
    if (!wanted.empty()) return SetError(error, "PullBlobs: peer did not send blob " + *wanted.begin()), false;
    return true;
}

std::optional<v1::TaskResult> NodeClient::Run(const std::string& task_id,
                                              const std::function<void(const v1::JobEvent&)>& on_event,
                                              std::string* error) {
    auto stub = PinnedStub(error);
    if (!stub) return std::nullopt;
    grpc::ClientContext ctx;  // no deadline: keepalive detects a vanished peer
    {
        std::lock_guard lock(mu_);
        if (runs_.contains(task_id)) return SetError(error, "task " + task_id + " is already running"), std::nullopt;
        runs_[task_id] = &ctx;
        cancelled_.erase(task_id);
    }
    v1::RunRequest req;
    req.set_task_id(task_id);
    std::optional<v1::TaskResult> result;
    auto reader = stub->RunTask(&ctx, req);
    v1::JobEvent ev;
    while (reader->Read(&ev)) {
        if (ev.has_result()) result = ev.result();
        if (on_event) on_event(ev);
        if (result) break;  // the final event
    }
    grpc::Status st = reader->Finish();
    bool cancelled = false;
    {
        std::lock_guard lock(mu_);
        runs_.erase(task_id);
        cancelled = cancelled_.erase(task_id) > 0;
    }
    if (result) return result;
    if (cancelled) {
        v1::TaskResult r;
        r.set_exit_code(-1);
        r.set_cancelled(true);
        r.set_error("cancelled");
        return r;
    }
    return SetError(error, Describe("RunTask", st)), std::nullopt;
}

void NodeClient::Cancel(const std::string& task_id) {
    {
        std::lock_guard lock(mu_);
        if (!runs_.contains(task_id)) return;
        cancelled_.insert(task_id);
    }
    bool told = false;
    if (auto stub = PinnedStub(nullptr)) {
        v1::TaskRef ref;
        ref.set_task_id(task_id);
        grpc::ClientContext ctx;
        SetDeadline(ctx);
        v1::Empty empty;
        told = stub->CancelTask(&ctx, ref, &empty).ok();
    }
    if (!told) {
        std::lock_guard lock(mu_);
        if (auto it = runs_.find(task_id); it != runs_.end()) it->second->TryCancel();
    }
}

bool NodeClient::Drain(const std::string& reason, std::string* error) {
    auto stub = PinnedStub(error);
    if (!stub) return false;
    v1::DrainNotice req;
    req.set_node_id(self_.node_id);
    req.set_reason(reason);
    grpc::ClientContext ctx;
    SetDeadline(ctx);
    v1::Empty empty;
    grpc::Status st = stub->Drain(&ctx, req, &empty);
    if (!st.ok()) return SetError(error, Describe("Drain", st)), false;
    return true;
}

} // namespace zeromesh::node
