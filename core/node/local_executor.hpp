#pragma once

#include "exec/sandbox_runner.hpp"
#include "node/executor.hpp"
#include "node/node_client.hpp"
#include "plugin/loader.hpp"
#include "store/store.hpp"

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

// Runs tasks on this node: a pool of `slots` workers, each resolving the task's environment
// provider from the Registry (Check, Prepare, Release) and running it in the SandboxRunner.
using NodeClientFactory = std::function<std::shared_ptr<NodeClient>(const std::string& node_id)>;

class LocalExecutor final : public Executor {
public:
    LocalExecutor(std::shared_ptr<exec::SandboxRunner> runner, std::shared_ptr<plugin::Registry> registry,
                  std::string node_id, int slots, std::filesystem::path scratch_root,
                  std::shared_ptr<store::CasStore> cas, NodeClientFactory client_factory);
    ~LocalExecutor() override;

    LocalExecutor(const LocalExecutor&) = delete;
    LocalExecutor& operator=(const LocalExecutor&) = delete;

    void Execute(const std::string& task_id, const v1::TaskSpec& spec, TaskCallbacks cb) override;
    // A queued task completes at once as cancelled; a running one has its sandbox killed.
    void Cancel(const std::string& task_id) override;

    // Cores held by sandboxes that have started and not finished.
    [[nodiscard]] int CoresInUse() const;

    // Snapshot of sandboxes on this node for zm monitor (queued/running + recent finished).
    struct ActiveItem {
        std::string task_id;
        v1::TaskSpec spec;
        bool started{false};
        v1::TaskState state{v1::QUEUED};
    };
    [[nodiscard]] std::vector<ActiveItem> ListActive() const;
    // Forgets finished sandboxes (monitor history only).
    void ClearFinished();

private:
    static constexpr size_t kRecentLimit = 128;

    struct Item {
        std::string task_id;
        v1::TaskSpec spec;
        TaskCallbacks cb;
        std::stop_source stop;
        bool started{false};
        int cores_held{1};
    };

    void Worker(std::stop_token st);
    v1::TaskResult Run(Item& item);
    void RememberFinished(std::string task_id, v1::TaskSpec spec, v1::TaskState state);

    std::shared_ptr<exec::SandboxRunner> runner_;
    std::shared_ptr<plugin::Registry> registry_;
    std::string node_id_;
    int core_capacity_{1};
    int cores_in_use_{0};
    std::filesystem::path scratch_root_;
    std::shared_ptr<store::CasStore> cas_;
    NodeClientFactory client_factory_;

    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    std::deque<std::shared_ptr<Item>> queue_;
    std::unordered_map<std::string, std::shared_ptr<Item>> items_;  // queued and running
    std::deque<ActiveItem> recent_;                                 // finished (monitor history)
    std::vector<std::jthread> workers_;                             // last: joined first
};

} // namespace zeromesh::node
