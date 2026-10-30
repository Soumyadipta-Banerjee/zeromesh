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
#include "node/executor.hpp"
#include "node/local_executor.hpp"
#include "node/node_client.hpp"
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

    // Node ids of remote peers that currently have at least one task in flight here.
    [[nodiscard]] std::vector<std::string> NodesWithInFlight() const;

    // Tasks currently reserved/running on `node_id` (for tests).
    [[nodiscard]] int InFlightOn(const std::string& node_id) const;

    // Payload bytes this requester has moved to / from each remote node.
    struct NodeTraffic {
        uint64_t bytes_pushed{0};
        uint64_t bytes_pulled{0};
    };
    [[nodiscard]] std::map<std::string, NodeTraffic> Traffic() const;

    struct Task;
    struct Shared;

private:
    void Place(const std::shared_ptr<Task>& task);
    // Runs locally and waits for the result (so Place can decide on retry).
    v1::TaskResult RunLocal(const std::shared_ptr<Task>& task, const v1::TaskSpec& spec);
    void OnPeerDrain(const std::string& node_id, const std::string& reason);
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
