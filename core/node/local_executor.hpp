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

// Runs batches on this node: workers take queued batches while cores (and declared memory) allow,
// prepare the batch's environment (host or tree provider), and hand it to the BatchRunner.
class LocalExecutor final : public Executor {
public:
    // `core_capacity` is how many cores batches may hold at once. Scratch, fetched environments and
    // warm directories live under `state_dir`.
    LocalExecutor(std::shared_ptr<exec::BatchRunner> runner, std::shared_ptr<plugin::Registry> registry,
                  std::string node_id, int core_capacity, std::filesystem::path state_dir,
                  std::shared_ptr<store::CasStore> cas, NodeClientFactory client_factory);
    ~LocalExecutor() override;

    LocalExecutor(const LocalExecutor&) = delete;
    LocalExecutor& operator=(const LocalExecutor&) = delete;

    // `exclude` is ignored: this executor is one node.
    void Execute(const std::string& batch_id, const v1::BatchSpec& spec, std::set<std::string> exclude,
                 BatchCallbacks cb) override;
    // A queued batch completes at once as cancelled; a running one is stopped (SIGTERM, then SIGKILL).
    void Cancel(const std::string& batch_id) override;

    [[nodiscard]] int CoresInUse() const;
    [[nodiscard]] int64_t MemInUse() const;
    [[nodiscard]] int CoreCapacity() const { return core_capacity_; }

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
        std::stop_source stop;
        bool started{false};
        int cores{1};
        int64_t mem{0}; // reservation
        std::shared_ptr<std::atomic<int64_t>> memory_now = std::make_shared<std::atomic<int64_t>>(0); // measured
        int done{0};
        std::string current;
    };

    void Worker(std::stop_token st);
    [[nodiscard]] bool MemoryAllowsLocked(const Item& item) const;
    BatchDone Run(Item& item);

    std::shared_ptr<exec::BatchRunner> runner_;
    std::shared_ptr<plugin::Registry> registry_;
    std::string node_id_;
    int core_capacity_{1};
    std::filesystem::path state_dir_;
    std::shared_ptr<store::CasStore> cas_;
    NodeClientFactory client_factory_;

    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    int cores_in_use_{0};
    int64_t mem_in_use_{0};
    std::chrono::steady_clock::time_point last_start_{};
    std::deque<std::shared_ptr<Item>> queue_;
    std::unordered_map<std::string, std::shared_ptr<Item>> items_; // queued and running
    std::vector<std::jthread> workers_;                            // last: joined first
};

} // namespace zeromesh::node
