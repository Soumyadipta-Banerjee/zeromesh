#include "node/node_client.hpp"

#include "net/tls.hpp"
#include "node/admission.hpp"

#include <grpcpp/client_context.h>

#include <stdexcept>

namespace zeromesh::node {

namespace {

void SetError(std::string* error, std::string msg) {
    if (error) *error = std::move(msg);
}

std::string Describe(const char* what, const grpc::Status& st) {
    return std::string(what) + " failed (code " + std::to_string(static_cast<int>(st.error_code())) +
           "): " + st.error_message();
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
        pinned_stub_ = v1::NodeApi::NewStub(net::PeerChannel(address_, net::ClientCreds(self_, accept)));
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

} // namespace zeromesh::node
