#pragma once

// NodeApi server (spec §6.1). Serve it only with net::ServerCreds. Admit and
// AdmitConfirm run the admission handshake (node/admission.hpp); every other
// RPC requires the caller's certificate fingerprint to be pinned.

#include "net/pin_store.hpp"
#include "net/team.hpp"
#include "node/admission.hpp"

#include "zeromesh/v1/node.grpc.pb.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace zeromesh::node {

class NodeService final : public v1::NodeApi::Service {
public:
    NodeService(std::string node_id, std::shared_ptr<net::PinStore> pins,
                std::function<std::optional<net::TeamKey>()> team, std::string self_fingerprint,
                std::function<v1::PeerInfo()> self_info);

    grpc::Status Admit(grpc::ServerContext* ctx, const v1::AdmitHello* req, v1::AdmitChallenge* resp) override;
    grpc::Status AdmitConfirm(grpc::ServerContext* ctx, const v1::AdmitProof* req, v1::Empty* resp) override;
    grpc::Status Ping(grpc::ServerContext* ctx, const v1::Empty* req, v1::PeerInfo* resp) override;

    // Pin-gated; UNIMPLEMENTED until Task 15.
    grpc::Status OfferTask(grpc::ServerContext* ctx, const v1::Offer* req, v1::OfferReply* resp) override;
    grpc::Status PushBlobs(grpc::ServerContext* ctx, grpc::ServerReader<v1::Chunk>* reader, v1::Empty* resp) override;
    grpc::Status PullBlobs(grpc::ServerContext* ctx, const v1::Digests* req,
                           grpc::ServerWriter<v1::Chunk>* writer) override;
    grpc::Status RunTask(grpc::ServerContext* ctx, const v1::RunRequest* req,
                         grpc::ServerWriter<v1::JobEvent>* writer) override;
    grpc::Status CancelTask(grpc::ServerContext* ctx, const v1::TaskRef* req, v1::Empty* resp) override;
    grpc::Status Drain(grpc::ServerContext* ctx, const v1::DrainNotice* req, v1::Empty* resp) override;

private:
    // The pin gate: OK (and the caller's pinned node id in *peer_node_id, if given)
    // iff the caller's TLS certificate fingerprint is pinned; else PERMISSION_DENIED.
    grpc::Status RequirePinned(const grpc::ServerContext& ctx, std::string* peer_node_id = nullptr) const;

    std::string node_id_;
    std::shared_ptr<net::PinStore> pins_;
    std::function<std::optional<net::TeamKey>()> team_;
    std::string self_fp_;
    std::function<v1::PeerInfo()> self_info_;
    PendingAdmissions pending_;
};

} // namespace zeromesh::node
