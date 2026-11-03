#pragma once

// The run engine: turns an adapter's run (a workspace, stages and items) into batches, and owns
// everything between "here are my items" and "every output is in place":
//
//   - items wait for the items they come after, whose outputs are then overlaid onto their inputs;
//   - each ready item is looked up in the item cache, here and then on the team's nodes; a hit is
//     placed at its destinations without running anything;
//   - the rest are packed into batches: longest first (by learned cost), sized so the live slots
//     stay busy without a long tail, consecutive items together where the stage combines them;
//   - batches go to the executor (the placer) one attempt at a time. Each item's outcome arrives as
//     soon as it finishes: its outputs are written to their destinations, it is cached, and its
//     cost and memory are learned;
//   - an item that failed retries by policy: a lost donor or a sandbox failure (infra retries,
//     never on the same node), a non-zero exit (exit retries), an OOM kill (with more memory, at
//     most twice). An item no node could run gets one try on this node (local fallback);
//   - checkpoints an item saved come back with the next attempt;
//   - a run ends when every item is done (sessions: once closed), on the first failure unless
//     keep_going, on cancel, or at its deadline or budget. A session no client has watched for
//     session_grace is cancelled: whoever was adding its items is gone. Its report is written to the
//     adapter's report_path and kept in the run history, with the run's spec so it can be resumed.

#include "node/executor.hpp"
#include "run/event_stream.hpp"
#include "run/item_cache.hpp"
#include "run/resources.hpp"
#include "run/team.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"
#include "zeromesh/v1/run.pb.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace zeromesh::run {

struct EngineDeps {
    std::shared_ptr<store::CasStore> cas;
    std::shared_ptr<store::MerkleBuilder> merkle;
    std::shared_ptr<ItemCache> cache;
    std::shared_ptr<node::Executor> executor;
    std::shared_ptr<ResourceModel> resources;
    std::shared_ptr<Team> team; // may be null: this node's cache only
    // Slots the team has right now for items needing `resources` placed by `where`.
    std::function<int(const v1::Resources&, v1::Where)> slots;
    std::function<std::string(const std::string& node)> hostname;
    std::filesystem::path runs_dir; // run history: <id>.json reports and <id>.spec
    std::string node_id;
    std::string arch;
    // How long an open run may go unwatched before it is cancelled: time for a session whose
    // process restarted to attach again.
    std::chrono::milliseconds session_grace{std::chrono::minutes(1)};
};

using RunEvents = EventStream<v1::RunEvent>;

class RunEngine {
public:
    explicit RunEngine(EngineDeps deps);
    ~RunEngine(); // cancels every running run

    RunEngine(const RunEngine&) = delete;
    RunEngine& operator=(const RunEngine&) = delete;

    // Throws std::invalid_argument for a malformed run (ids, stages, paths, dependency cycles,
    // a missing workspace, an exact program this node does not have).
    std::string Submit(v1::RunSpec spec);
    void AddItems(const std::string& run_id, std::vector<v1::Item> items); // open runs only
    void Close(const std::string& run_id);
    void Cancel(const std::string& run_id);
    // Resubmits a past run's spec as a new run.
    std::string Resume(const std::string& run_id);

    [[nodiscard]] std::optional<v1::RunStatus> Status(const std::string& run_id) const;
    // The report of a run in memory or in the history.
    [[nodiscard]] std::optional<v1::Report> Report(const std::string& run_id) const;
    // nullptr for an unknown run. A finished run replays its events and ends.
    std::shared_ptr<RunEvents> Watch(const std::string& run_id, bool from_start);
    // Runs in memory (running first), then the history, newest first.
    [[nodiscard]] std::vector<v1::RunStatus> List() const;

    // Inputs of running runs and outputs of runs finished in the last few minutes: GC keeps these.
    [[nodiscard]] std::unordered_set<std::string> ProtectedBlobHashes() const;

    struct Run;
    struct Item;

private:
    using RunPtr = std::shared_ptr<Run>;

    void Loop(std::stop_token st);
    // Returns true if it did anything.
    bool Pump(const RunPtr& run);
    void Lookup(const RunPtr& run, std::vector<size_t> items);
    void Dispatch(const RunPtr& run);
    void OnItem(const RunPtr& run, const std::string& batch, const v1::ItemOutcome& o, const std::string& node);
    void OnBatchDone(const RunPtr& run, const std::string& batch, node::BatchDone d);
    void Finish(const RunPtr& run);
    void WriteReport(const RunPtr& run, const v1::Report& report);

    // The following need mu_.
    void AddItemsLocked(Run& run, std::vector<v1::Item> items);
    void Emit(Run& run, v1::RunEvent ev, bool keep = true);
    void Update(Run& run, size_t i, v1::ItemState state, const std::string& node = {}, const std::string& detail = {});
    void Terminal(Run& run, size_t i, v1::ItemState state, v1::Failure failure);
    void Requeue(Run& run, size_t i, const std::string& why);
    void Fail(Run& run, size_t i, v1::FailureKind kind, const std::string& message, int exit_code = 0,
              const std::string& out_tail = {}, const std::string& err_tail = {});
    void CancelLocked(Run& run, const std::string& ended_by);
    [[nodiscard]] v1::RunStatus StatusLocked(const Run& run) const;
    [[nodiscard]] v1::Report ReportLocked(const Run& run) const;
    void Wake();

    // Executor callbacks may outlive the engine (a hedge copy reporting late): each holds this and
    // runs only while the engine is alive; the destructor waits out the ones in progress.
    struct Guard {
        std::shared_mutex mu;
        bool dead{false};
    };

    EngineDeps deps_;
    std::shared_ptr<Guard> guard_ = std::make_shared<Guard>();
    mutable std::mutex mu_;
    std::condition_variable_any cv_;
    bool dirty_{false};
    std::map<std::string, RunPtr> runs_;
    std::vector<std::string> order_; // submit order
    std::jthread loop_;              // last: stopped and joined first
};

} // namespace zeromesh::run
