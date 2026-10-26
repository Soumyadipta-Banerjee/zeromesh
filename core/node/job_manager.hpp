#pragma once

#include "node/action_cache.hpp"
#include "node/executor.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"
#include "zeromesh/v1/common.pb.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::node {

// The events of one job for one watcher: past events first, then live ones. Next returns
// nullopt on timeout, after Close, or once the job is done and every event was read.
class EventStream {
public:
    std::optional<v1::JobEvent> Next(std::chrono::milliseconds timeout);
    void Close();

    // Used by JobManager.
    void Push(const v1::JobEvent& ev);
    void End();
    [[nodiscard]] bool closed() const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<v1::JobEvent> queue_;
    bool ended_{false};
    bool closed_{false};
};

// Runs jobs (static task DAGs). A task starts once all its dependencies (depends_on and
// inputs_from tasks) succeeded; its effective input_root is its own input_root with every
// inputs_from task's outputs overlaid at `path`. A successful ActionCache lookup finishes the
// task as a cache hit; otherwise it goes to the Executor. Thread-safe.
class JobManager {
public:
    JobManager(std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
               std::shared_ptr<ActionCache> cache, std::shared_ptr<Executor> executor);
    ~JobManager();

    JobManager(const JobManager&) = delete;
    JobManager& operator=(const JobManager&) = delete;

    // Throws std::invalid_argument for empty, duplicate or unknown task names and cycles.
    std::string Submit(v1::JobSpec spec);
    void Cancel(const std::string& job_id);
    [[nodiscard]] std::optional<v1::JobStatus> Get(const std::string& job_id) const;
    // Snapshot of submitted jobs for zm monitor (working + finished tasks stay listed).
    [[nodiscard]] std::vector<v1::ActiveTask> ListActive() const;
    // nullptr for an unknown job.
    std::shared_ptr<EventStream> Watch(const std::string& job_id);

    class Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace zeromesh::node
