#pragma once

#include "exec/sandbox_runner.hpp"
#include "node/executor.hpp"
#include "plugin/loader.hpp"

#include <condition_variable>
#include <deque>
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
class LocalExecutor final : public Executor {
public:
    LocalExecutor(std::shared_ptr<exec::SandboxRunner> runner, std::shared_ptr<plugin::Registry> registry,
                  std::string node_id, int slots);
    ~LocalExecutor() override;

    LocalExecutor(const LocalExecutor&) = delete;
    LocalExecutor& operator=(const LocalExecutor&) = delete;

    void Execute(const std::string& task_id, const v1::TaskSpec& spec, TaskCallbacks cb) override;
    // A queued task completes at once as cancelled; a running one has its sandbox killed.
    void Cancel(const std::string& task_id) override;

    // Snapshot of sandboxes on this node for zm monitor (queued/running + recent finished).
    struct ActiveItem {
        std::string task_id;
        v1::TaskSpec spec;
        bool started{false};
        v1::TaskState state{v1::QUEUED};
    };
    [[nodiscard]] std::vector<ActiveItem> ListActive() const;

private:
    static constexpr size_t kRecentLimit = 128;

    struct Item {
        std::string task_id;
        v1::TaskSpec spec;
        TaskCallbacks cb;
        std::stop_source stop;
        bool started{false};
    };

    void Worker(std::stop_token st);
    v1::TaskResult Run(Item& item);
    void RememberFinished(std::string task_id, v1::TaskSpec spec, v1::TaskState state);

    std::shared_ptr<exec::SandboxRunner> runner_;
    std::shared_ptr<plugin::Registry> registry_;
    std::string node_id_;

    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    std::deque<std::shared_ptr<Item>> queue_;
    std::unordered_map<std::string, std::shared_ptr<Item>> items_;  // queued and running
    std::deque<ActiveItem> recent_;                                 // finished (monitor history)
    std::vector<std::jthread> workers_;                             // last: joined first
};

} // namespace zeromesh::node
