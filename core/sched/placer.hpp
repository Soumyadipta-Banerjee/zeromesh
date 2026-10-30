#pragma once

// Placer (spec §7 step 3, §8, M4): the Executor JobManager uses. It ranks this node
// and the active team peers for each task and tries them in order:
//   - local: hand the task to the LocalExecutor;
//   - remote: OfferTask (a rejection moves on to the next candidate), push the
//     inputs the peer lacks, RunTask while forwarding its events, then pull the
//     outputs (whole trees for directory outputs) and stdout/stderr into the local
//     CAS before reporting the result.
// Capacity is reserved atomically before Offer/Run so concurrent tasks do not all
// pile onto the top-ranked node (at most cores_total/task.cores in flight per node;
// load-derived cores_free is a ranking/UI signal only). Donor drain, peer loss, and
// push/pull transport failures re-queue up to policy.max_retries (JobEvent QUEUED,
// detail "retry n/max: reason"). Run timeouts, non-zero exits, and donor env/policy
// rejections are final; user/job cancel is never retried.

#include "net/peer_table.hpp"
#include "net/telemetry.hpp"
#include "node/donor_policy.hpp"
#include "node/executor.hpp"
#include "node/local_executor.hpp"
#include "node/node_client.hpp"
#include "sched/seed_gate.hpp"
#include "sched/hazard.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <thread>
#include <unordered_set>
#include <vector>

namespace zeromesh::sched {

class Placer final : public node::Executor {
public:
    // A client for a peer from the PeerTable, or nullptr if none can be made. Clients may
    // be shared between tasks (NodeClient is thread-safe).
    using ClientFactory = std::function<std::shared_ptr<node::NodeClient>(const net::Peer&)>;

    // `local` may be null (this node never runs tasks itself). `telemetry` describes this
    // node; an empty resources.arch is filled with its arch before ranking and offering.
    // Registers a PeerTable preempt callback so a donor drain cancels in-flight remote runs
    // and re-queues them.
    Placer(std::shared_ptr<node::LocalExecutor> local, std::shared_ptr<net::PeerTable> peers, ClientFactory factory,
           std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
           std::string local_node_id, std::function<net::Telemetry()> telemetry);
    // Cancels every task still placing or running remotely and waits for them.
    ~Placer() override;

    Placer(const Placer&) = delete;
    Placer& operator=(const Placer&) = delete;

    void Execute(const std::string& task_id, const v1::TaskSpec& spec, node::TaskCallbacks cb) override;
    void Cancel(const std::string& task_id) override;

    // Rank donors by expected committed work (slots x E[min(remaining life, task time)]) from
    // this departure-hazard model. Without a model, or until it has any history, placement
    // ranks by free slots as before. Safe to call while tasks are being placed.
    void SetHazardModel(std::shared_ptr<const HazardModel> model);

    // Node ids of remote peers that currently have at least one task in flight here.
    [[nodiscard]] std::vector<std::string> NodesWithInFlight() const;

    // Tasks currently reserved/running on `node_id` (for tests).
    [[nodiscard]] int InFlightOn(const std::string& node_id) const;

    // Payload bytes this requester has moved to / from each remote node.
    struct NodeTraffic {
        uint64_t bytes_pushed{0};
        uint64_t bytes_pulled{0};
        uint64_t bytes_seeded{0}; // input bytes the node fetched from another donor instead of from us
    };
    [[nodiscard]] std::map<std::string, NodeTraffic> Traffic() const;

    // Peer seeding: a donor that misses a blob fetches it from the least-busy donor already holding
    // it (SeedBlobs) instead of the requester uploading it again. Anything not seeded is pushed.
    struct SeedConfig {
        bool enabled{true};
        int per_source_cap{SeedGate::kDefaultPerSource}; // concurrent sends from one donor
        int lan_cap{SeedGate::kDefaultLanWide};          // concurrent seed transfers on the whole LAN
        std::chrono::milliseconds acquire_timeout{2000}; // wait this long for a slot, then push instead
    };
    void SetSeeding(SeedConfig cfg);

    // Tail hedging: near the end of a job, copy a task that sits on a donor unlikely to
    // outlive it onto a second donor. The first copy to succeed wins and the others are
    // cancelled. Off until `survival` is set (the hazard model supplies it).
    struct HedgeConfig {
        // P(the donor is still here `horizon_s` seconds from now); unset means never hedge.
        std::function<double(const std::string& node_id, double horizon_s)> survival;
        double survive_below{0.5};  // hedge when the odds fall below this
        int max_copies{2};          // simultaneous copies of one task, original included
        int tail_max_running{4};    // "tail": nothing waiting for a slot and at most this many tasks running
        double default_horizon_s{120.0}; // assumed remaining runtime; capped by the task's timeout
        std::chrono::milliseconds check_interval{200};
        std::chrono::milliseconds retry_after{1000}; // pause between two hedge launches for one task

        static HedgeConfig FromPolicy(const node::DonorPolicy& policy);
    };
    struct HedgeStats {
        uint64_t launched{0};  // hedge copies that obtained a donor slot
        uint64_t won{0};       // hedge copies whose result was used
        uint64_t cancelled{0}; // hedge copies stopped because another copy finished first
        double cpu_seconds{0};        // run time x cores of every hedge copy
        double wasted_cpu_seconds{0}; // the part of it spent by copies that did not win
    };
    void SetHedging(HedgeConfig cfg);
    [[nodiscard]] HedgeStats Hedging() const;

    struct Task;
    struct Shared;

private:
    void Place(const std::shared_ptr<Task>& task);
    // Runs locally and waits for the result (so Place can decide on retry).
    v1::TaskResult RunLocal(const std::shared_ptr<Task>& task, const v1::TaskSpec& spec);
    void OnPeerDrain(const std::string& node_id, const std::string& reason);
    void LaunchWorker(const std::shared_ptr<Task>& task);
    // Lets `target` fetch what it misses from donors known to hold it; returns what is still missing.
    std::vector<v1::Digest> SeedFromPeers(const std::string& target, node::NodeClient& client,
                                          std::vector<v1::Digest> missing);
    void NoteHolders(const std::string& node_id, const std::vector<v1::Digest>& digests);
    void AddSeeded(const std::string& node_id, uint64_t bytes);
    void HedgeScan();
    void LaunchHedge(const std::shared_ptr<Task>& primary, std::unordered_set<std::string> exclude,
                     const HedgeConfig& cfg);
    void OnHedgeDone(const std::shared_ptr<Task>& primary, const std::shared_ptr<Task>& hedge, v1::TaskResult r);
    void CancelHedges(const std::shared_ptr<Task>& primary);
    void ReapFinishedLocked();

    [[nodiscard]] int64_t InputTreeBytes(const v1::TaskSpec& spec) const;
    [[nodiscard]] int64_t EstimatedMissingBytes(const std::string& node_id, const std::string& input_root_hash,
                                                int64_t total_bytes) const;
    void NoteInputsPushed(const std::string& node_id, const std::string& input_root_hash, int64_t bytes_pushed,
                          int64_t total_bytes);

    void AddTraffic(const std::string& node_id, uint64_t pushed, uint64_t pulled);

    // Blobs recently pushed to a donor. Offers made before a sibling task's push lands all
    // report the same blobs as missing; the push lock plus this set sends each one once.
    struct DonorSent {
        std::mutex push_mu;
        std::mutex mu;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> sent;
    };
    std::shared_ptr<DonorSent> SentFor(const std::string& node_id);
    static constexpr std::chrono::seconds kSentTtl{30};

    std::mutex sent_mu_;
    std::map<std::string, std::shared_ptr<DonorSent>> sent_;

    mutable std::mutex seed_mu_;
    SeedConfig seed_cfg_;
    SeedGate seed_gate_;
    std::mutex holders_mu_;
    std::unordered_map<std::string, std::vector<std::string>> holders_; // blob hash -> donors known to hold it
    static constexpr size_t kHoldersMaxBlobs = 200000;
    static constexpr size_t kHoldersPerBlob = 8;

    mutable std::mutex hedge_mu_;
    HedgeConfig hedge_cfg_;
    HedgeStats hedge_stats_;
    std::jthread hedge_thread_;
    [[nodiscard]] std::shared_ptr<const HazardModel> Hazard() const;
    mutable std::mutex hazard_mu_;
    std::shared_ptr<const HazardModel> hazard_;

    mutable std::mutex traffic_mu_;
    std::map<std::string, NodeTraffic> traffic_;

    mutable std::mutex push_cache_mu_;
    std::unordered_map<std::string, int64_t> push_cache_; // "node|root" -> bytes known on peer
    std::list<std::string> push_cache_lru_;
    static constexpr size_t kPushCacheMax = 128;

    std::shared_ptr<node::LocalExecutor> local_;
    std::shared_ptr<net::PeerTable> peers_;
    ClientFactory factory_;
    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::string local_id_;
    std::function<net::Telemetry()> telemetry_;
    std::shared_ptr<Shared> shared_;

    struct Worker {
        std::shared_ptr<Task> task;
        std::jthread thread;
    };
    std::mutex workers_mu_;
    std::list<Worker> workers_;
};

} // namespace zeromesh::sched
