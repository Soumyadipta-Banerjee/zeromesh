#pragma once

// cgroup v2 helpers for task sandboxes. zmd finds the deepest cgroup along its own ancestry where
// it may create children (a systemd user session delegates one), moves itself into a "zm-daemon"
// leaf there, and creates one "zm-task-*" cgroup per batch beside it. A batch always gets a cgroup
// when delegation exists, with whichever limits the delegated controllers allow: the cgroup is what
// lets a departing donor freeze the batch, the comfort governor throttle it, and the runner see
// OOM kills and CPU progress, even when no limit could be applied.

#include <sys/types.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace zeromesh::exec::cgroup {

// The parent task cgroups are created under; nullopt without cgroup v2 delegation. The first call
// moves this process into <parent>/zm-daemon (the no-internal-processes rule requires it).
std::optional<std::filesystem::path> TaskParent();

// The deepest cgroup along our ancestry that this process may create children in, without moving
// anything (a probe for tests and diagnostics).
std::optional<std::filesystem::path> DelegatedParent();

struct Group {
    std::filesystem::path dir;
    bool memory{false}; // memory.max applied
    bool cpu{false};    // cpu.max applied
};

// A fresh child of `parent` with memory.max = mem_bytes (0: none) and cpu.max = cores (0: none)
// where those controllers are delegated. nullopt when the directory cannot be created.
std::optional<Group> Create(const std::filesystem::path& parent, int cores, uint64_t mem_bytes);
void Remove(const std::filesystem::path& dir);

// Moves the calling process into `dir` (call in the child between fork and exec).
void Join(const std::filesystem::path& dir);

[[nodiscard]] uint64_t OomKills(const std::filesystem::path& dir);
[[nodiscard]] bool Frozen(const std::filesystem::path& dir);
bool SetFrozen(const std::filesystem::path& dir, bool frozen);
// Total CPU time of the cgroup (cpu.stat usage_usec), present even without the cpu controller.
[[nodiscard]] std::optional<uint64_t> CpuUsageUsec(const std::filesystem::path& dir);
// Memory of the cgroup now (memory.current) and the most it has used (memory.peak, Linux 5.19+).
[[nodiscard]] std::optional<uint64_t> MemoryCurrent(const std::filesystem::path& dir);
[[nodiscard]] std::optional<uint64_t> MemoryPeak(const std::filesystem::path& dir);
[[nodiscard]] std::vector<pid_t> Procs(const std::filesystem::path& dir);
// SIGKILLs every process in the cgroup at once (cgroup.kill); false when unsupported.
bool KillAll(const std::filesystem::path& dir);

// Whether limits can be enforced here: a throwaway cgroup is created and removed.
struct Enforcement {
    bool memory{false};
    bool cpu{false};
};
[[nodiscard]] Enforcement Probe();

} // namespace zeromesh::exec::cgroup
