#include "node/node_service.hpp"

#include "net/identity.hpp"
#include "net/tls.hpp"

#include <stdexcept>

namespace zeromesh::node {

namespace {

grpc::Status Denied(const std::string& why) { return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, why); }

grpc::Status NotYet() { return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "not implemented yet"); }

std::string MacBytes(const std::array<uint8_t, 32>& mac) { return std::string(mac.begin(), mac.end()); }

} // namespace

NodeService::NodeService(std::string node_id, std::shared_ptr<net::PinStore> pins,
                         std::function<std::optional<net::TeamKey>()> team, std::string self_fingerprint,
                         std::function<v1::PeerInfo()> self_info)
    : node_id_(std::move(node_id)),
      pins_(std::move(pins)),
      team_(std::move(team)),
      self_fp_(std::move(self_fingerprint)),
      self_info_(std::move(self_info)) {
    if (!pins_ || !team_ || !self_info_) throw std::invalid_argument("NodeService: missing dependency");
    if (!net::IsFingerprint(self_fp_)) throw std::invalid_argument("NodeService: bad self fingerprint");
}

grpc::Status NodeService::RequirePinned(const grpc::ServerContext& ctx, std::string* peer_node_id) const {
    std::string fp = net::PeerFingerprint(ctx);
    if (fp.empty()) return Denied("no client certificate");
    auto node = pins_->NodeFor(fp);
    if (!node) return Denied("peer is not admitted to this team");
    if (peer_node_id) *peer_node_id = std::move(*node);
    return grpc::Status::OK;
}

grpc::Status NodeService::Admit(grpc::ServerContext* ctx, const v1::AdmitHello* req, v1::AdmitChallenge* resp) {
    auto key = team_();
    if (!key) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "node is not in a team");
    std::string fp_client = net::PeerFingerprint(*ctx);
    if (fp_client.empty()) return Denied("no client certificate");
    if (!net::IsValidNodeId(req->node_id())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid node id");
    }
    if (req->nonce().size() != kAdmitNonceBytes) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "nonce must be 32 bytes");
    }

    std::string nonce_s = AdmitNonce();
    resp->set_node_id(node_id_);
    resp->set_nonce(nonce_s);
    resp->set_mac(MacBytes(AdmitServerMac(*key, req->nonce(), self_fp_, fp_client)));
    pending_.Begin(fp_client, req->node_id(), std::move(nonce_s));
    return grpc::Status::OK;
}

grpc::Status NodeService::AdmitConfirm(grpc::ServerContext* ctx, const v1::AdmitProof* req, v1::Empty* /*resp*/) {
    std::string fp_client = net::PeerFingerprint(*ctx);
    if (fp_client.empty()) return Denied("no client certificate");
    auto entry = pending_.Take(fp_client); // single use, MAC match or not
    if (!entry) return Denied("no pending admission for this certificate");
    auto key = team_();
    if (!key) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "node is not in a team");
    if (!AdmitMacMatches(req->mac(), AdmitClientMac(*key, entry->nonce_s, fp_client, self_fp_))) {
        return Denied("admission proof does not match this team");
    }
    try {
        pins_->Pin(entry->node_id, fp_client);
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("cannot pin peer: ") + e.what());
    }
    return grpc::Status::OK;
}

grpc::Status NodeService::Ping(grpc::ServerContext* ctx, const v1::Empty* /*req*/, v1::PeerInfo* resp) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    *resp = self_info_();
    return grpc::Status::OK;
}

grpc::Status NodeService::OfferTask(grpc::ServerContext* ctx, const v1::Offer* /*req*/, v1::OfferReply* /*resp*/) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    return NotYet();
}

grpc::Status NodeService::PushBlobs(grpc::ServerContext* ctx, grpc::ServerReader<v1::Chunk>* /*reader*/,
                                    v1::Empty* /*resp*/) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    return NotYet();
}

grpc::Status NodeService::PullBlobs(grpc::ServerContext* ctx, const v1::Digests* /*req*/,
                                    grpc::ServerWriter<v1::Chunk>* /*writer*/) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    return NotYet();
}

grpc::Status NodeService::RunTask(grpc::ServerContext* ctx, const v1::RunRequest* /*req*/,
                                  grpc::ServerWriter<v1::JobEvent>* /*writer*/) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    return NotYet();
}

grpc::Status NodeService::CancelTask(grpc::ServerContext* ctx, const v1::TaskRef* /*req*/, v1::Empty* /*resp*/) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    return NotYet();
}

grpc::Status NodeService::Drain(grpc::ServerContext* ctx, const v1::DrainNotice* /*req*/, v1::Empty* /*resp*/) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    return NotYet();
}

} // namespace zeromesh::node
