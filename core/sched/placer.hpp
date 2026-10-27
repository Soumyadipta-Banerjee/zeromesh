#pragma once

// Placer (spec §7 step 3, §8, M4): the Executor JobManager uses. Tasks wait in their
// job's queue; one dispatcher thread asks the job's dispatch policy (JobSpec.dispatch,
// built-in or IDispatchPolicy plugin) for (task, node) pairs whenever a slot frees, a
// task is queued or the mesh changes, reserves that capacity, and starts a worker that
// runs Offer → Push → Run → Pull there (or the LocalExecutor for this node). A task that
// needs another node (retry, donor full, command missing) goes back to the front of its
// queue. Donor drain, peer loss, and push/pull failures retry up to policy.max_retries
// (JobEvent QUEUED "retry n/max: reason"); run timeouts, non-zero exits and env/policy
// rejections are final; user/job cancel is never retried.

#include "net/peer_table.hpp"
#include "net/telemetry.hpp"
#include "node/executor.hpp"
#include "node/local_executor.hpp"
#include "node/node_client.hpp"
#include "plugin/loader.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zeromesh::sched {

class Placer final : public node::Executor {
public:
    using ClientFactory = std::function<std::shared_ptr<node::NodeClient>(const net::Peer&)>;

    Placer(std::shared_ptr<node::LocalExecutor> local, std::shared_ptr<net::PeerTable> peers, ClientFactory factory,
           std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
           std::shared_ptr<plugin::Registry> registry, std::string local_node_id,
           std::function<net::Telemetry()> telemetry);
    ~Placer() override;

    Placer(const Placer&) = delete;
    Placer& operator=(const Placer&) = delete;

    void Execute(const std::string& task_id, const v1::TaskSpec& spec, node::TaskCallbacks cb,
                 node::ExecuteContext ctx = {}) override;
    void Cancel(const std::string& task_id) override;

    [[nodiscard]] std::vector<std::string> NodesWithInFlight() const;
    [[nodiscard]] int InFlightOn(const std::string& node_id) const;

    struct Task;
    struct Shared;

private:
    struct JobQueue {
        std::string job_id;
        v1::DispatchPolicy dispatch;
        std::deque<std::shared_ptr<Task>> pending;
        int in_flight{0};  // tasks of this job reserved or running
    };

    void DispatcherLoop(std::stop_token st);
    void DispatchOnce();
    void WakeDispatcher();
    // Runs one dispatched task on task->place_on (already reserved). Returns true when
    // the task needs another node and was put back in its job's queue.
    bool RunPlacement(const std::shared_ptr<Task>& task);
    void Requeue(const std::shared_ptr<Task>& task);
    void OnPeerDrain(const std::string& node_id, const std::string& reason);
    void ReapFinishedLocked();
    v1::TaskResult RunLocal(const std::shared_ptr<Task>& task, const v1::TaskSpec& spec);

    [[nodiscard]] int64_t InputTreeBytes(const v1::TaskSpec& spec) const;
    [[nodiscard]] int64_t EstimatedMissingBytes(const std::string& node_id, const std::string& input_root_hash,
                                                int64_t total_bytes) const;
    void NoteInputsPushed(const std::string& node_id, const std::string& input_root_hash, int64_t bytes_pushed,
                          int64_t total_bytes);

    mutable std::mutex push_cache_mu_;
    std::unordered_map<std::string, int64_t> push_cache_;
    std::list<std::string> push_cache_lru_;
    static constexpr size_t kPushCacheMax = 128;

    std::shared_ptr<node::LocalExecutor> local_;
    std::shared_ptr<net::PeerTable> peers_;
    ClientFactory factory_;
    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::shared_ptr<plugin::Registry> registry_;
    std::string local_id_;
    std::function<net::Telemetry()> telemetry_;
    std::shared_ptr<Shared> shared_;

    std::mutex dispatch_mu_;
    std::condition_variable_any dispatch_cv_;
    bool dispatch_dirty_{true};  // guarded by dispatch_mu_
    std::map<std::string, JobQueue> jobs_;

    struct Worker {
        std::shared_ptr<std::atomic<bool>> done;  // per worker: a requeued task gets a new one
        std::jthread thread;
    };
    void StartWorker(const std::shared_ptr<Task>& task);
    std::mutex workers_mu_;
    std::list<Worker> workers_;
    std::jthread dispatcher_;  // last: stops first
};

} // namespace zeromesh::sched
