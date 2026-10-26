#pragma once

#include "net/discovery.hpp"
#include "net/peer_table.hpp"
#include "net/pin_store.hpp"
#include "net/team.hpp"
#include "net/telemetry.hpp"
#include "node/job_manager.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"
#include "zeromesh/v1/client.grpc.pb.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace zeromesh::node {

inline constexpr uint64_t kDefaultUploadLimitBytes = 2ULL * 1024 * 1024 * 1024;  // 2 GiB
inline constexpr size_t kObjectChunkBytes = 1024 * 1024;                          // 1 MiB

// kDefaultUploadLimitBytes, or $ZM_UPLOAD_LIMIT_BYTES when it is a positive integer.
// The override exists for integration tests, so they need not upload 2 GiB.
uint64_t UploadLimitFromEnv();

struct ClientServiceOptions {
    std::string node_id;
    int node_port{8981};
    uint64_t upload_limit_bytes{kDefaultUploadLimitBytes};
};

// Shared team membership state for ClientService Team* RPCs and NodeService Admit.
// Owned by the Daemon; Create/Join/Leave update the file, discovery and pins.
struct TeamState {
    std::filesystem::path state_dir;
    std::shared_ptr<net::PinStore> pins;
    std::shared_ptr<net::Discovery> discovery;
    std::mutex mu;
    std::optional<net::TeamKey> key;
    // Fired after membership changes (clear the admitted-client cache).
    std::function<void()> on_changed;

    [[nodiscard]] std::optional<net::TeamKey> Get();
    // Replaces membership. Clears pins, updates the team file and Discovery::SetTeam.
    void Apply(std::optional<net::TeamKey> next);
};

// zeromesh.v1.ClientApi (spec §6.1), served by zmd on its owner-only unix socket. The client
// runs on the same machine, so UploadTree and FetchOutputs take local paths.
class ClientService final : public v1::ClientApi::Service {
public:
    ClientService(std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
                  std::shared_ptr<JobManager> jobs, std::shared_ptr<net::PeerTable> peers,
                  ClientServiceOptions options, std::shared_ptr<TeamState> team = nullptr);

    grpc::Status PutObjects(grpc::ServerContext* ctx, grpc::ServerReader<v1::Chunk>* reader,
                            v1::PutObjectsReply* reply) override;
    grpc::Status GetObjects(grpc::ServerContext* ctx, const v1::Digests* req,
                            grpc::ServerWriter<v1::Chunk>* writer) override;
    grpc::Status FindMissing(grpc::ServerContext* ctx, const v1::Digests* req, v1::Digests* reply) override;
    grpc::Status UploadTree(grpc::ServerContext* ctx, const v1::UploadTreeRequest* req,
                            v1::UploadTreeReply* reply) override;
    grpc::Status SubmitJob(grpc::ServerContext* ctx, const v1::JobSpec* req, v1::SubmitReply* reply) override;
    grpc::Status WatchJob(grpc::ServerContext* ctx, const v1::JobRef* req,
                          grpc::ServerWriter<v1::JobEvent>* writer) override;
    grpc::Status GetJob(grpc::ServerContext* ctx, const v1::JobRef* req, v1::JobStatus* reply) override;
    grpc::Status CancelJob(grpc::ServerContext* ctx, const v1::JobRef* req, v1::Empty* reply) override;
    grpc::Status FetchOutputs(grpc::ServerContext* ctx, const v1::FetchRequest* req, v1::FetchReply* reply) override;
    grpc::Status ListPeers(grpc::ServerContext* ctx, const v1::Empty* req, v1::PeerList* reply) override;
    grpc::Status TeamCreate(grpc::ServerContext* ctx, const v1::Empty* req, v1::TeamSecretReply* reply) override;
    grpc::Status TeamJoin(grpc::ServerContext* ctx, const v1::TeamJoinRequest* req, v1::Empty* reply) override;
    grpc::Status TeamLeave(grpc::ServerContext* ctx, const v1::Empty* req, v1::Empty* reply) override;
    grpc::Status TeamInfo(grpc::ServerContext* ctx, const v1::Empty* req, v1::TeamInfoReply* reply) override;

    // This node's PeerInfo (is_self = true) from fresh telemetry.
    v1::PeerInfo SelfInfo();

private:
    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::shared_ptr<JobManager> jobs_;
    std::shared_ptr<net::PeerTable> peers_;
    ClientServiceOptions options_;
    std::shared_ptr<TeamState> team_;

    std::mutex telemetry_mu_;
    net::TelemetrySampler telemetry_;
};

} // namespace zeromesh::node
