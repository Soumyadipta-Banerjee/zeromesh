#pragma once

// cgroup v2 containment of the work a node runs.
//
// zmd needs its own cgroup delegated to it with the memory, pids and cpu controllers. Its systemd
// unit gets one through Delegate=; a zmd started any other way asks the user's systemd for a scope
// with Delegate=yes (EnsureDelegatedScope). There zmd lives in a leaf, and all work runs under one
// subtree whose memory.max is the node's memory budget: however wrong an estimate is, work cannot
// take the machine's memory, an OOM kill stays inside the subtree, and zmd keeps answering.
//
//   <delegated>/zm-daemon            zmd itself (processes may only live in leaves)
//   <delegated>/zm-work              memory.max = the budget, no swap
//   <delegated>/zm-work/b-<id>       one batch: cpu.max, memory.max (a declared limit), oom.group, pids.max
//   <delegated>/zm-work/b-<id>/<p>   one process of the batch: its exact memory peak, CPU time, OOM kills
//
// The batch's cgroup is also what a departing donor freezes and what the comfort governor throttles.

#include <sys/types.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::exec::cgroup {

// Why work cannot be contained in this process's cgroup; empty when it can.
std::string ContainmentProblem();

// When ContainmentProblem() is not empty and systemd can give this user a scope with Delegate=yes,
// re-executes the program in one: same pid, arguments and environment. Tries once per launch and
// returns when nothing was re-executed. Call first in main, before any thread starts.
void EnsureDelegatedScope(char** argv);

struct Work {
    std::filesystem::path root; // where batches go; empty when work cannot be contained here
    std::string problem;        // why not, when root is empty
    uint64_t budget{0};         // root's memory.max in bytes; 0: none below the machine's own
};

// Moves this process into <delegated>/zm-daemon and makes <delegated>/zm-work with memory.max =
// budget_bytes (0: none). Later calls return the same root with the new budget applied.
Work SetUpWork(uint64_t budget_bytes);

// A batch's cgroup under `work`: cpu.max = cores (0: none), memory.max = mem_limit (0: none),
// memory.oom.group = 1 (an OOM kill takes all of the batch's processes) and pids.max, with the
// memory controller enabled for its children. nullopt (and *error) when any of that cannot be done.
std::optional<std::filesystem::path> CreateBatch(const std::filesystem::path& work, int cores, uint64_t mem_limit,
                                                 std::string* error = nullptr);

// A child of a batch's cgroup for one process and its descendants. nullopt (and *error) when it
// cannot be made.
std::optional<std::filesystem::path> CreateChild(const std::filesystem::path& batch, const std::string& name,
                                                 std::string* error = nullptr);

// Kills what is left in `dir` and below, waits up to `wait` for it to empty, then removes the
// cgroups bottom-up. False when something could not be removed.
bool Remove(const std::filesystem::path& dir, std::chrono::milliseconds wait = std::chrono::seconds(5));

// cgroup.procs of `dir`, opened for writing (close-on-exec), or -1. A forked child joins `dir` by
// writing "0" to it: no allocation between fork and exec.
int OpenProcs(const std::filesystem::path& dir);

[[nodiscard]] uint64_t OomKills(const std::filesystem::path& dir);
// Times the cgroup's own memory.max stopped an allocation (memory.events.local "oom"): the limit of
// this cgroup was hit, not one of an ancestor.
[[nodiscard]] uint64_t LimitOoms(const std::filesystem::path& dir);
[[nodiscard]] bool Frozen(const std::filesystem::path& dir);
bool SetFrozen(const std::filesystem::path& dir, bool frozen);
// CPU time of the cgroup and below (cpu.stat usage_usec, present without the cpu controller too).
[[nodiscard]] std::optional<uint64_t> CpuUsageUsec(const std::filesystem::path& dir);
// The most memory the cgroup and below have used together (memory.peak, Linux 5.19+).
[[nodiscard]] std::optional<uint64_t> MemoryPeak(const std::filesystem::path& dir);
[[nodiscard]] std::vector<pid_t> Procs(const std::filesystem::path& dir);
// SIGKILLs every process in the cgroup and below at once (cgroup.kill, Linux 5.14+).
bool KillAll(const std::filesystem::path& dir);

} // namespace zeromesh::exec::cgroup
