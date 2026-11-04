#pragma once

// Runs a batch of items on this node, each in its own bubblewrap sandbox.
//
// The batch's input tree is materialized once (InputCache) and shared read-only. Every item (or,
// for a combined batch, the one process that runs them all) sees it at /work as the lower layer of
// a fresh overlay, so its writes land in a private upper directory: the store is never written
// through, and "what the item changed" is exactly what the upper directory holds. Without overlay
// support the tree is copied per item instead.
//
// Work is contained (exec/cgroup.hpp): the batch runs in its own cgroup under the node's work
// cgroup, each process in a child of it, so an item's memory and CPU are the kernel's exact
// figures and no batch can take more memory than the node's budget. A node that cannot contain
// work runs none (unavailable()).
//
// Each item gets its own exit code, logs, outputs, CPU time and peak memory. A combined batch
// commits each item the moment its declared outputs are closed, so a donor that leaves mid-batch
// keeps every finished item. Items may write checkpoints to /checkpoint; each change is stored and
// reported, and the next attempt starts with the last one. Cancellation sends SIGTERM to the
// item's processes, then SIGKILL after a grace period.

#include "exec/cgroup.hpp"
#include "exec/input_cache.hpp"
#include "plugin/plugin.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zeromesh::exec {

inline constexpr size_t kLogTailBytes = 4096;

struct ItemSpec {
    std::string id;
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env;
    std::string cwd{"."};                      // relative to /work
    std::vector<std::string> outputs;          // relative to cwd; missing ones fail the item
    std::vector<std::string> optional_outputs; // relative to cwd
    bool capture_changed{false};               // also every file created or modified under cwd
    std::string depfile;                       // relative to cwd: Make-style list of what it read
    int64_t seq{0};                            // {first}/{last} in a combined command
    store::Digest checkpoint;                  // restored into /checkpoint (empty: none)
};

struct BatchRunSpec {
    store::Digest input_root;
    plugin::SandboxEnv sandbox_env;                       // from the environment provider
    std::vector<std::pair<std::string, std::string>> env; // for every item, before item env
    std::vector<ItemSpec> items;
    // Non-empty: one process runs every item, with "{first}" and "{last}" replaced by the first and
    // last item's seq. All items then share items[0].cwd.
    std::vector<std::string> combine_argv;
    int cores{0};                         // the batch's cgroup cpu.max (0: no cap)
    uint64_t mem_bytes{0};                // the batch's cgroup memory.max, a declared limit (0: only the budget)
    std::vector<std::string> gpu_devices; // host device nodes to expose (empty: no GPU)
    bool network{false};
    std::chrono::seconds timeout{3600}; // per item
    std::chrono::seconds hung_after{0}; // no CPU progress and no output for this long: hung (0: off)
    std::chrono::seconds term_grace{5}; // SIGTERM to SIGKILL on cancel
    std::filesystem::path warm_dir;     // mounted writable at /warm when set
    std::string node_id;                // ZM_NODE_ID
    std::string tag;                    // names the batch to CgroupFor / SetFrozen
    bool stream_logs{false};
};

struct OutputEntry {
    std::string path;     // relative to the item's cwd
    store::Digest digest; // blob for files, tree for directories, empty for symlinks
    bool is_dir{false};
    bool is_executable{false};
    bool is_symlink{false};
    std::string symlink_target;
};

enum class ItemFailure { None, Exit, MissingOutput, Oom, Timeout, Hung, Sandbox };

struct ItemRunResult {
    std::string id;
    int exit_code{-1};
    ItemFailure failure{ItemFailure::None};
    std::string message;
    std::vector<OutputEntry> outputs;
    store::Digest stdout_digest;
    store::Digest stderr_digest;
    std::string stdout_tail; // last kLogTailBytes
    std::string stderr_tail;
    int64_t wall_ms{0};
    int64_t cpu_ms{0};
    int64_t peak_mem{0};                // bytes the item's processes held at most, together
    bool oom_at_limit{false};           // Oom: its batch's own limit was hit, not the node's budget
    int64_t budget{0};                  // Oom not at the limit: what all work on this node may use
    std::vector<std::string> footprint; // workspace-relative files the depfile names
};

struct BatchCallbacks {
    std::function<void(const std::string& item)> on_started;
    std::function<void(const std::string& item, int fd, std::string_view chunk)> on_log; // stream_logs only
    std::function<void(ItemRunResult)> on_item;
    std::function<void(const std::string& item, const store::Digest& tree)> on_checkpoint;
};

struct BatchRunResult {
    std::string error;     // the batch could not run at all (no bwrap, no containment, inputs missing, ...)
    bool cancelled{false}; // stopped before every item finished
};

class BatchRunner {
public:
    // Scratch, overlay layers and the input cache live under `scratch_base`; batches run in cgroups
    // under `work` (cgroup::SetUpWork).
    BatchRunner(std::shared_ptr<store::CasStore> store, std::shared_ptr<store::MerkleBuilder> merkle,
                std::filesystem::path scratch_base, cgroup::Work work);

    // Runs the items in order until all are done or `stop` is requested. on_item is called once for
    // every item that ran to an outcome; items not reached (stop, sandbox failure) get no call.
    BatchRunResult Run(const BatchRunSpec& spec, const BatchCallbacks& cb, std::stop_token stop,
                       std::shared_ptr<InputCache::Lease> input = {});

    std::shared_ptr<InputCache::Lease> CachedInput(const store::Digest& root) { return inputs_.AcquireCached(root); }

    // Why batches cannot run here (no sandbox, or work cannot be contained); empty when they can.
    [[nodiscard]] const std::string& unavailable() const { return unavailable_; }
    // Why bubblewrap cannot sandbox items here (missing, refused or too old); empty when it can.
    [[nodiscard]] const std::string& sandbox_problem() const { return bwrap_error_; }
    // Where work is contained, its memory budget, or why it cannot be.
    [[nodiscard]] const cgroup::Work& work() const { return work_; }
    // Whether inputs reach items through an overlay (otherwise they are copied per item).
    [[nodiscard]] bool overlay() const { return overlay_; }

    // Called with (cgroup dir, true) once a batch's cgroup exists and (dir, false) before it is
    // removed, so the comfort governor can throttle running work.
    using CgroupObserver = std::function<void(const std::filesystem::path&, bool added)>;
    void SetCgroupObserver(CgroupObserver observer);
    [[nodiscard]] std::optional<std::filesystem::path> CgroupFor(const std::string& tag) const;
    // Freezes or thaws the batch started with `tag`; frozen time does not count against timeouts.
    bool SetFrozen(const std::string& tag, bool frozen);

    // Drops input trees no batch has used for `idle`.
    void PruneInputs(std::chrono::seconds idle);

    // Stops the item (or combined group) that started last as out of memory: called when the work
    // cgroup is stalled on memory (node/work_pressure.hpp). False when nothing runs.
    bool StopYoungestForMemory();

private:
    struct Group;
    void RunGroup(const BatchRunSpec& spec, Group& g, const std::filesystem::path& lower,
                  const std::filesystem::path& batch_cgroup, const BatchCallbacks& cb, std::stop_token stop,
                  bool& cancelled);
    bool ProbeOverlay();

    std::shared_ptr<store::CasStore> store_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::filesystem::path scratch_base_;
    InputCache inputs_;
    cgroup::Work work_;
    cgroup::Reaper reaper_; // a finished item's or batch's cgroup, removed beside the next
    std::string bwrap_path_;
    std::string bwrap_error_;
    std::string unavailable_;
    bool overlay_{false};
    CgroupObserver cgroup_observer_;
    // What runs now, one entry per process group: its cgroup, when it started, and whether it was
    // stopped for memory.
    struct Live {
        std::filesystem::path cgroup;
        std::chrono::steady_clock::time_point started;
        std::shared_ptr<std::atomic<bool>> stopped_for_memory;
    };
    std::mutex live_mu_;
    std::map<uint64_t, Live> live_;
    uint64_t live_seq_{0};
    mutable std::mutex tagged_mu_;
    std::map<std::string, std::filesystem::path> tagged_;
};

} // namespace zeromesh::exec
