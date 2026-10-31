#pragma once

#include "exec/batch_runner.hpp"
#include "node/executor.hpp"
#include "node/node_client.hpp"
#include "plugin/loader.hpp"
#include "store/store.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeromesh::node {

using NodeClientFactory = std::function<std::shared_ptr<NodeClient>(const std::string& node_id)>;

// Runs batches on this node: workers take queued batches, first come first served, once their cores
// and memory reservation fit what is not yet reserved, prepare the batch's environment (host or tree
// provider), and hand it to the BatchRunner. The kernel holds all work to the memory budget
// (exec/cgroup.hpp), so admission counts reservations against it and never guesses from what is
// free this second.
//
// Every batch this node takes is held first (Hold): a donor's accepted offer, a batch its own placer
// puts here. A hold succeeds only when the batch fits beside everything held, queued and running, so
// a held batch starts as soon as it is handed over, and no batch waits here that another node could
// run.
class LocalExecutor final : public Executor {
public:
    // `core_capacity` is how many cores batches may hold at once; the memory budget is the runner's
    // (runner->work().budget, 0: none). Scratch, fetched environments and warm directories live
    // under `state_dir`.
    LocalExecutor(std::shared_ptr<exec::BatchRunner> runner, std::shared_ptr<plugin::Registry> registry,
                  std::string node_id, int core_capacity, std::filesystem::path state_dir,
                  std::shared_ptr<store::CasStore> cas, NodeClientFactory client_factory);
    ~LocalExecutor() override;

    LocalExecutor(const LocalExecutor&) = delete;
    LocalExecutor& operator=(const LocalExecutor&) = delete;

    // `exclude` is ignored: this executor is one node. A batch held under `batch_id` goes ahead of
    // any that was not; one handed over without a hold waits in the queue until it fits.
    void Execute(const std::string& batch_id, const v1::BatchSpec& spec, std::set<std::string> exclude,
                 BatchCallbacks cb) override;
    // Reserves the cores and memory of a batch that Execute hands over shortly under the same id:
    // true when they fit beside everything held, queued and running; nothing else can take them
    // then. False also for an id already held or here.
    bool Hold(const std::string& batch_id, const v1::BatchSpec& spec,
              std::shared_ptr<exec::InputCache::Lease> input = {});
    std::shared_ptr<exec::InputCache::Lease> CachedInput(const v1::Digest& root) const;
    // Gives back a hold whose batch never came. A no-op once it was handed over.
    void Release(const std::string& batch_id);
    // A queued batch completes at once as cancelled; a running one is stopped (SIGTERM, then SIGKILL).
    void Cancel(const std::string& batch_id) override;

    // Cores and memory reserved here: held, queued or running.
    [[nodiscard]] int CoresReserved() const;
    [[nodiscard]] int64_t MemReserved() const;
    [[nodiscard]] int CoreCapacity() const { return core_capacity_; }
    // The memory all batches here may hold together; 0: no budget.
    [[nodiscard]] int64_t MemoryBudget() const { return mem_budget_; }
    // Why this node cannot run batches (no sandbox, no containment); empty when it can.
    [[nodiscard]] const std::string& Unavailable() const;

    struct Active {
        std::string batch_id;
        v1::BatchSpec spec;
        bool running{false};
        int done{0};
        std::string current;
    };
    [[nodiscard]] std::vector<Active> ListActive() const;

private:
    struct Item {
        std::string batch_id;
        v1::BatchSpec spec;
        BatchCallbacks cb;
        std::shared_ptr<exec::InputCache::Lease> input;
        std::stop_source stop;
        bool started{false};
        int cores{1};
        int64_t mem{0};           // what it holds of the memory budget (HeldMemory)
        bool never_fits{false};   // needs more memory than the whole budget
        int done{0};
        std::string current;
    };

    [[nodiscard]] std::shared_ptr<Item> MakeItem(const std::string& batch_id, const v1::BatchSpec& spec) const;
    void Worker(std::stop_token st);
    [[nodiscard]] int CoresReservedLocked() const;
    [[nodiscard]] int64_t MemReservedLocked() const;
    [[nodiscard]] bool FitsLocked(const Item& item) const;
    BatchDone Run(Item& item);

    std::shared_ptr<exec::BatchRunner> runner_;
    std::shared_ptr<plugin::Registry> registry_;
    std::string node_id_;
    int core_capacity_{1};
    int64_t mem_budget_{0};
    std::filesystem::path state_dir_;
    std::shared_ptr<store::CasStore> cas_;
    NodeClientFactory client_factory_;

    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    int cores_in_use_{0}; // by running batches
    int64_t mem_in_use_{0};
    std::deque<std::shared_ptr<Item>> queue_;
    std::unordered_map<std::string, std::shared_ptr<Item>> items_; // queued and running
    std::unordered_map<std::string, std::shared_ptr<Item>> held_;  // held, not yet handed over
    int held_cores_{0};
    int64_t held_mem_{0};
    std::vector<std::jthread> workers_; // last: joined first
};

} // namespace zeromesh::node
