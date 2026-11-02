#include "exec/cgroup.hpp"

#include <fcntl.h>
#include <sodium.h>
#include <unistd.h>

#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace zeromesh::exec::cgroup {

namespace {

namespace fs = std::filesystem;

constexpr const char* kDaemonCgroupName = "zm-daemon";
constexpr const char* kControllers[] = {"memory", "cpu", "pids", "io"};

std::string RandomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    randombytes_buf(raw.data(), raw.size());
    std::string out;
    for (unsigned char c : raw) {
        out.push_back("0123456789abcdef"[c >> 4]);
        out.push_back("0123456789abcdef"[c & 15]);
    }
    return out;
}

std::string ReadSmallFile(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

bool WriteSmallFile(const fs::path& p, const std::string& data) {
    std::ofstream out(p);
    if (!out) return false;
    out << data;
    out.flush();
    return static_cast<bool>(out);
}

bool Writable(const fs::path& p) {
    return access(p.c_str(), W_OK) == 0;
}

bool Listed(const std::string& haystack, const std::string& name) {
    std::istringstream in(haystack);
    std::string word;
    while (in >> word) {
        if (word == name) return true;
    }
    return false;
}

// Enables every available controller of kControllers for `parent`'s children.
bool EnableSubtreeControllers(const fs::path& parent) {
    const std::string avail = ReadSmallFile(parent / "cgroup.controllers");
    const std::string enabled = ReadSmallFile(parent / "cgroup.subtree_control");
    std::string add;
    for (const char* c : kControllers) {
        if (Listed(avail, c) && !Listed(enabled, c)) add += std::string("+") + c + " ";
    }
    return add.empty() || WriteSmallFile(parent / "cgroup.subtree_control", add);
}

std::optional<fs::path> SelfCgroup() {
    std::ifstream cg("/proc/self/cgroup");
    std::string line;
    while (std::getline(cg, line)) {
        if (line.rfind("0::/", 0) == 0) return fs::path("/sys/fs/cgroup") / line.substr(4);
    }
    return std::nullopt;
}

// Keyed by the field name, e.g. "oom_kill" in memory.events or "usage_usec" in cpu.stat.
std::optional<uint64_t> ReadKeyed(const fs::path& file, const std::string& key) {
    std::ifstream in(file);
    std::string k;
    uint64_t v = 0;
    while (in >> k >> v) {
        if (k == key) return v;
    }
    return std::nullopt;
}

} // namespace

std::optional<fs::path> DelegatedParent() {
    auto cur = SelfCgroup();
    if (!cur) return std::nullopt;
    const fs::path root = "/sys/fs/cgroup";
    for (;;) {
        const fs::path probe = *cur / (".zm-cg-probe-" + RandomHex(4));
        std::error_code ec;
        if (fs::create_directory(probe, ec)) {
            fs::remove(probe, ec);
            // A child we can create is only useful if we may also write the controller files of
            // the parent (to enable controllers) or the child's own limits.
            if (Writable(*cur / "cgroup.subtree_control")) return *cur;
        }
        if (*cur == root || cur->parent_path() == *cur) break;
        cur = cur->parent_path();
    }
    return std::nullopt;
}

std::optional<fs::path> TaskParent() {
    static std::once_flag once;
    static std::optional<fs::path> parent;
    std::call_once(once, [] {
        auto p = DelegatedParent();
        if (!p) return;
        const fs::path leaf = *p / kDaemonCgroupName;
        std::error_code ec;
        fs::create_directory(leaf, ec);
        if (ec) return;
        // Processes may not live in a cgroup that has controllers enabled for its children.
        Join(leaf);
        if (!EnableSubtreeControllers(*p)) {
            // Still usable for freezing and accounting; limits will simply not apply.
        }
        parent = *p;
    });
    return parent;
}

std::optional<Group> Create(const fs::path& parent, int cores, uint64_t mem_bytes) {
    EnableSubtreeControllers(parent);
    Group g;
    g.dir = parent / ("zm-task-" + RandomHex(8));
    std::error_code ec;
    if (!fs::create_directory(g.dir, ec)) return std::nullopt;
    if (mem_bytes > 0 && Writable(g.dir / "memory.max") &&
        WriteSmallFile(g.dir / "memory.max", std::to_string(mem_bytes))) {
        if (Writable(g.dir / "memory.swap.max")) WriteSmallFile(g.dir / "memory.swap.max", "0");
        g.memory = true;
    }
    if (cores > 0 && Writable(g.dir / "cpu.max") &&
        WriteSmallFile(g.dir / "cpu.max", std::to_string(static_cast<int64_t>(cores) * 100000) + " 100000")) {
        g.cpu = true;
    }
    if (Writable(g.dir / "pids.max")) WriteSmallFile(g.dir / "pids.max", "4096");
    return g;
}

void Remove(const fs::path& dir) {
    std::error_code ec;
    fs::remove(dir, ec);
}

void Join(const fs::path& dir) {
    const fs::path procs = dir / "cgroup.procs";
    int fd = open(procs.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    const std::string pid = std::to_string(getpid()) + "\n";
    (void)!write(fd, pid.data(), pid.size());
    close(fd);
}

uint64_t OomKills(const fs::path& dir) {
    return ReadKeyed(dir / "memory.events", "oom_kill").value_or(0);
}

bool Frozen(const fs::path& dir) {
    return ReadKeyed(dir / "cgroup.events", "frozen").value_or(0) == 1;
}

bool SetFrozen(const fs::path& dir, bool frozen) {
    const fs::path file = dir / "cgroup.freeze";
    return Writable(file) && WriteSmallFile(file, frozen ? "1" : "0");
}

std::optional<uint64_t> CpuUsageUsec(const fs::path& dir) {
    return ReadKeyed(dir / "cpu.stat", "usage_usec");
}

namespace {
std::optional<uint64_t> ReadNumber(const fs::path& file) {
    std::ifstream in(file);
    uint64_t v = 0;
    if (in >> v) return v;
    return std::nullopt;
}
} // namespace

std::optional<uint64_t> MemoryCurrent(const fs::path& dir) {
    return ReadNumber(dir / "memory.current");
}

std::optional<uint64_t> MemoryPeak(const fs::path& dir) {
    return ReadNumber(dir / "memory.peak");
}

std::vector<pid_t> Procs(const fs::path& dir) {
    std::vector<pid_t> out;
    std::ifstream in(dir / "cgroup.procs");
    pid_t p = 0;
    while (in >> p) out.push_back(p);
    return out;
}

bool KillAll(const fs::path& dir) {
    const fs::path file = dir / "cgroup.kill";
    return Writable(file) && WriteSmallFile(file, "1");
}

Enforcement Probe() {
    Enforcement e;
    auto parent = TaskParent();
    if (!parent) return e;
    auto g = Create(*parent, 1, 64 * 1024 * 1024);
    if (!g) return e;
    e.memory = g->memory;
    e.cpu = g->cpu;
    Remove(g->dir);
    return e;
}

} // namespace zeromesh::exec::cgroup
