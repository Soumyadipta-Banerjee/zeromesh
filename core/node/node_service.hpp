#pragma once

// NodeApi server (spec §6.1). Serve it only with net::ServerCreds (and
// net::AllowKeepalivePings). Admit and AdmitConfirm run the admission handshake
// (node/admission.hpp); every other RPC first requires the caller's certificate
// fingerprint to be pinned.
//
// Remote execution: OfferTask checks the env provider, the donor policy and the
// inputs this node lacks; an accepted offer reserves the task for 30 s, keyed by
// (caller node id, task id). The caller pushes the missing blobs (PushBlobs, each
// digest verified) and calls RunTask, which runs the reserved spec through the
// LocalExecutor (so ZM_NODE_ID is this node's id) and streams JobEvents; the last
// event carries the TaskResult. Outputs stay in this node's CAS for PullBlobs.

#include "net/peer_table.hpp"
#include "net/pin_store.hpp"
#include "net/team.hpp"
#include "net/telemetry.hpp"
#include "node/admission.hpp"
#include "node/departure.hpp"
#include "node/donor_policy.hpp"
#include "node/local_executor.hpp"
#include "plugin/loader.hpp"
#include "store/store.hpp"

#include "zeromesh/v1/node.grpc.pb.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace zeromesh::node {

class NodeService final : public v1::NodeApi::Service {
public:
    NodeService(std::string node_id, std::shared_ptr<net::PinStore> pins,
                std::function<std::optional<net::TeamKey>()> team, std::string self_fingerprint,
                std::function<v1::PeerInfo()> self_info, std::shared_ptr<store::CasStore> cas = nullptr,
                std::shared_ptr<plugin::Registry> registry = nullptr, std::shared_ptr<LocalExecutor> executor = nullptr,
                std::function<net::Telemetry()> telemetry = nullptr, DonorPolicy policy = {},
                std::shared_ptr<net::PeerTable> peers = nullptr);
    ~NodeService() override;

    NodeService(const NodeService&) = delete;
    NodeService& operator=(const NodeService&) = delete;

    grpc::Status Admit(grpc::ServerContext* ctx, const v1::AdmitHello* req, v1::AdmitChallenge* resp) override;
    grpc::Status AdmitConfirm(grpc::ServerContext* ctx, const v1::AdmitProof* req, v1::Empty* resp) override;
    grpc::Status Ping(grpc::ServerContext* ctx, const v1::Empty* req, v1::PeerInfo* resp) override;

    // Pin-gated. Without the matching dependency (cas/registry/executor/telemetry for
    // offers, blobs and runs; peers for Drain) they return UNIMPLEMENTED.
    grpc::Status OfferTask(grpc::ServerContext* ctx, const v1::Offer* req, v1::OfferReply* resp) override;
    grpc::Status PushBlobs(grpc::ServerContext* ctx, grpc::ServerReader<v1::Chunk>* reader, v1::Empty* resp) override;
    grpc::Status PullBlobs(grpc::ServerContext* ctx, const v1::Digests* req,
                           grpc::ServerWriter<v1::Chunk>* writer) override;
    grpc::Status SeedBlobs(grpc::ServerContext* ctx, const v1::SeedRequest* req, v1::SeedReply* resp) override;
    grpc::Status RunTask(grpc::ServerContext* ctx, const v1::RunRequest* req,
                         grpc::ServerWriter<v1::JobEvent>* writer) override;
    grpc::Status CancelTask(grpc::ServerContext* ctx, const v1::TaskRef* req, v1::Empty* resp) override;
    grpc::Status Drain(grpc::ServerContext* ctx, const v1::DrainNotice* req, v1::Empty* resp) override;

    // Sticky donor drain: refuse new offers while draining. Entering drain cancels
    // preemptible running tasks; non-preemptible ones keep running.
    void SetDraining(bool draining, std::string reason = {});

    // The donor's current comfort-ladder step (0..4), reported in every OfferReply. From step
    // kSqueezedLevel on, new offers are refused: the owner is being held back already.
    static constexpr int kSqueezedLevel = 3;
    void SetComfortLevelProvider(std::function<int()> provider);

    // The donor's own departure hazard (departures per hour), reported in every OfferReply as
    // hazard_per_hour. A negative or non-finite value means "no record yet" and leaves the field unset.
    void SetHazardProvider(std::function<double()> provider);
    [[nodiscard]] bool IsDraining() const;

    // How this node reaches another team peer by node id, so SeedBlobs can fetch from it. Without
    // one, every seed request is answered "no route" and the requester pushes the blobs itself.
    void SetPeerClientFactory(NodeClientFactory factory);

    // Node ids of requesters that currently have a reserved or running task here.
    [[nodiscard]] std::vector<std::string> RequestersWithWork() const;

    // Digests of reserved and running remote inputs. GC must not delete these.
    [[nodiscard]] std::unordered_set<std::string> ProtectedBlobHashes() const;

    // Cancels every running remote task whose Policy.preemptible is true (default).
    // Returns how many were cancelled.
    int CancelPreemptible();

    // ---- freeze-first departure (see node/departure.hpp) ----
    // Pauses or resumes one running task by its execution id ("<caller>/<task id>"); false when
    // it cannot (no cgroup freezer). The daemon wires this to the task's cgroup.
    using TaskFreezer = std::function<bool(const std::string& exec_id, bool freeze)>;
    void SetTaskFreezer(TaskFreezer freezer);
    // Past run times, used to tell which tasks will finish inside the grace window. Finished
    // tasks are recorded here. Without it every task counts as "unknown" and is frozen.
    void SetRuntimeHistory(std::shared_ptr<RuntimeHistory> history);

    struct DepartureReport {
        int kept{0};      // run on through the grace window (or are not preemptible)
        int frozen{0};    // paused until the ttl
        int cancelled{0}; // could not be frozen (or ttl is 0): killed at once, as before
    };
    // The donor is leaving: refuse new work, keep the tasks that fit `grace`, freeze the rest
    // for `ttl`. ttl == 0, or a freezer that fails, falls back to killing preemptible tasks.
    DepartureReport BeginDeparture(v1::DepartureReason reason, std::chrono::milliseconds ttl,
                                   std::chrono::milliseconds grace,
                                   std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // Call about once a second while departing: freezes kept tasks when the grace window is over
    // and kills frozen tasks whose ttl ran out (the requester then re-dispatches them).
    // Returns how many tasks it killed.
    int TickDeparture(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    // The donor is back: thaw every frozen task and accept work again. Returns how many it thawed.
    int EndDeparture();
    [[nodiscard]] bool Departing() const;

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

    // Remote execution.
    using TaskKey = std::pair<std::string, std::string>; // (caller node id, task id)
    struct Reservation {
        v1::TaskSpec spec;
        std::vector<store::Digest> needed;
        std::chrono::steady_clock::time_point expires;
    };
    struct Running {
        std::string exec_id;
        int cores{1};
        bool preemptible{true};
        v1::TaskSpec spec;
        std::vector<store::Digest> needed;
        std::chrono::steady_clock::time_point started{};
    };
    struct Departure {
        bool active{false};
        std::chrono::milliseconds ttl{0};
        std::chrono::steady_clock::time_point grace_ends{};
        std::set<std::string> kept; // preemptible tasks running through the grace window
        std::map<std::string, std::chrono::steady_clock::time_point> frozen; // exec id -> kill deadline
    };
    void PurgeExpiredLocked();
    int CoresInUseLocked() const;
    bool RemoteExecConfigured() const;

    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<plugin::Registry> registry_;
    std::shared_ptr<LocalExecutor> executor_;
    std::function<net::Telemetry()> telemetry_;
    DonorPolicy policy_;
    std::shared_ptr<net::PeerTable> peers_;

    mutable std::mutex peer_factory_mu_;
    NodeClientFactory peer_client_factory_;
    mutable std::mutex comfort_mu_;
    std::function<int()> comfort_level_;
    std::function<double()> hazard_per_hour_;
    mutable std::mutex mu_;
    std::map<TaskKey, Reservation> reserved_;
    std::map<TaskKey, Running> running_;
    bool draining_{false};
    std::string drain_reason_;
    Departure departure_;
    TaskFreezer freezer_;
    std::shared_ptr<RuntimeHistory> history_;
};

inline constexpr std::chrono::seconds kOfferReservation{30};

} // namespace zeromesh::node
