#pragma once

#include "plugin/plugin.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

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

inline constexpr size_t kLogHeadBytes = 64 * 1024;

struct RunSpec {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env;
    store::Digest input_root;         // empty hash: start with an empty /work
    std::string working_dir{"."};     // relative to /work
    std::vector<std::string> outputs; // paths relative to /work
    bool capture_changed{false};      // also capture every new or modified file under /work
    std::chrono::seconds timeout{3600};
    bool allow_network{false};
    plugin::SandboxEnv sandbox_env; // from the task's environment provider
    std::string node_id;            // exported as ZM_NODE_ID
    int cores{0};                   // 0 means 1 when applying a CPU cgroup quota
    uint64_t mem_bytes{0};          // 0 means no memory cap
    // Names the run to SandboxRunner::CgroupFor / SetTaskFrozen (empty: not addressable).
    std::string task_tag;
    // Partial commit (exec/units.hpp): called, from the thread that runs the sandbox, once for
    // each new unit file the task commits to $ZM_UNITS_DIR, with the unit stored in the CAS.
    // Units already present in the input tree are not reported.
    std::function<void(const std::string& unit_id, const store::Digest& digest)> on_unit;
};

struct OutputEntry {
    std::string path;
    store::Digest digest; // blob for files, tree for directories, empty for symlinks
    bool is_dir{false};
    bool is_executable{false};
    bool is_symlink{false};
    std::string symlink_target;
};

struct RunResult {
    int exit_code{-1};
    bool timed_out{false};
    bool cancelled{false};
    store::Digest stdout_digest;
    store::Digest stderr_digest;
    std::string stdout_head; // first kLogHeadBytes bytes
    std::string stderr_head;
    std::vector<OutputEntry> outputs;
    std::chrono::milliseconds duration{0};
    std::string error; // set when the sandbox itself could not run
    bool memory_enforced{false};
    bool cpu_enforced{false};
    std::string resource_note; // e.g. CPU not capped without cgroups
};

// Whether this process can create per-task cgroups (walks up from /proc/self/cgroup).
struct ResourceEnforcementStatus {
    bool cgroup_memory{false};
    bool cgroup_cpu{false};
    bool rlimit_memory{true};
};
[[nodiscard]] ResourceEnforcementStatus ProbeSandboxResourceEnforcement();

// The deepest cgroup along our ancestry that this process may create limited children in
// (nullopt without cgroup v2 delegation).
[[nodiscard]] std::optional<std::filesystem::path> DelegatedCgroupParent();

// fd is 1 (stdout) or 2 (stderr). May be empty.
using LogSink = std::function<void(int fd, std::string_view chunk)>;

// Runs one command inside bubblewrap: the input tree is materialized into a private scratch
// directory and bind-mounted at /work, only /work and /tmp are writable, there is no network
// unless allowed, and the child runs under SCHED_IDLE. Full stdout/stderr and outputs are
// stored in CAS. The scratch directory is always removed.
class SandboxRunner {
public:
    SandboxRunner(std::shared_ptr<store::CasStore> store, std::shared_ptr<store::MerkleBuilder> merkle,
                  std::filesystem::path scratch_base);

    RunResult Run(const RunSpec& spec, const LogSink& sink, std::stop_token stop);

    // Called with (cgroup dir, true) once a task's cgroup exists and (dir, false) just before
    // it is removed, so the comfort governor can throttle running tasks.
    using CgroupObserver = std::function<void(const std::filesystem::path&, bool added)>;
    void SetCgroupObserver(CgroupObserver observer);

    // The cgroup of the running task started with RunSpec::task_tag == `tag`; nullopt when no
    // such task runs or it has no cgroup (no delegation).
    [[nodiscard]] std::optional<std::filesystem::path> CgroupFor(const std::string& tag) const;
    // Freezes or thaws that task with cgroup.freeze. False when there is no such task cgroup
    // or the kernel refused. Time spent frozen does not count against the task's timeout.
    bool SetTaskFrozen(const std::string& tag, bool frozen);

private:
    std::shared_ptr<store::CasStore> store_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    std::filesystem::path scratch_base_;
    std::string bwrap_path_;
    CgroupObserver cgroup_observer_;
    mutable std::mutex tagged_mu_;
    std::map<std::string, std::filesystem::path> tagged_cgroups_;
    std::string bwrap_error_; // why bwrap_path_ was refused, if it was
};

} // namespace zeromesh::exec
