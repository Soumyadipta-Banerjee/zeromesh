#include "exec/cgroup.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sodium.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <sstream>

extern char** environ;

namespace zeromesh::exec::cgroup {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* kDaemonCgroup = "zm-daemon";
constexpr const char* kWorkCgroup = "zm-work";
// What containment needs: memory (the budget, OOM kills, accounting), pids (a fork bomb stays
// small), cpu (a batch's cores, the comfort governor).
constexpr std::initializer_list<const char*> kRequired = {"memory", "pids", "cpu"};
// Set across the one re-execution into a delegated scope, so a launch never loops.
constexpr const char* kScopedEnv = "ZM_IN_DELEGATED_SCOPE";
constexpr uint64_t kBatchPids = 4096;

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

// Writes `data` to a control file in one write(2); the error, or empty.
std::string Write(const fs::path& p, const std::string& data) {
    const int fd = open(p.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return p.string() + ": " + std::strerror(errno);
    const ssize_t n = write(fd, data.data(), data.size());
    const int err = n < 0 ? errno : EIO;
    close(fd);
    if (n != static_cast<ssize_t>(data.size())) return p.string() + ": " + std::strerror(err);
    return {};
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

std::optional<fs::path> SelfCgroup() {
    std::ifstream cg("/proc/self/cgroup");
    std::string line;
    while (std::getline(cg, line)) {
        if (line.rfind("0::/", 0) == 0) return fs::path("/sys/fs/cgroup") / line.substr(4);
    }
    return std::nullopt;
}

// The cgroup delegated to this process: its own, or the one holding its zm-daemon leaf.
std::optional<fs::path> Delegated() {
    auto own = SelfCgroup();
    if (!own) return std::nullopt;
    return own->filename() == kDaemonCgroup ? own->parent_path() : *own;
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

std::optional<uint64_t> ReadNumber(const fs::path& file) {
    std::ifstream in(file);
    uint64_t v = 0;
    if (in >> v) return v;
    return std::nullopt;
}

// Enables `controllers` for the children of `dir`; the error, or empty.
std::string EnableForChildren(const fs::path& dir, std::initializer_list<const char*> controllers) {
    const std::string enabled = ReadSmallFile(dir / "cgroup.subtree_control");
    std::string add;
    for (const char* c : controllers) {
        if (!Listed(enabled, c)) add += (add.empty() ? "+" : " +") + std::string(c);
    }
    if (add.empty()) return {};
    if (std::string err = Write(dir / "cgroup.subtree_control", add); !err.empty()) return "cannot enable " + add + ": " + err;
    return {};
}

// Runs `args` with no output; true when it exits 0.
bool Succeeds(const std::vector<std::string>& args) {
    std::vector<char*> cargv;
    for (const auto& a : args) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return false;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

std::string ContainmentProblem() {
    const auto dir = Delegated();
    if (!dir) return "this machine does not use cgroup v2";
    const std::string have = ReadSmallFile(*dir / "cgroup.controllers");
    std::string missing;
    for (const char* c : kRequired) {
        if (!Listed(have, c)) missing += (missing.empty() ? "" : ", ") + std::string(c);
    }
    if (!missing.empty()) return dir->string() + " has no " + missing + " controller for zmd";
    if (!Writable(*dir / "cgroup.subtree_control") || !Writable(*dir / "cgroup.procs")) {
        return dir->string() + " is not delegated to this user";
    }
    // Controllers are enabled for children only in a cgroup no process lives in: zmd moves to a
    // leaf, so it must be alone there.
    for (pid_t p : Procs(*dir)) {
        if (p != getpid()) return dir->string() + " is shared with other processes (pid " + std::to_string(p) + ")";
    }
    return {};
}

void EnsureDelegatedScope(char** argv) {
    if (std::getenv(kScopedEnv) != nullptr) { // re-executed already: never loop
        unsetenv(kScopedEnv);
        return;
    }
    if (ContainmentProblem().empty()) return;
    std::vector<std::string> run{"systemd-run"};
    if (geteuid() != 0) run.emplace_back("--user");
    for (const char* a : {"--scope", "--quiet", "--collect", "--property=Delegate=yes", "--"}) run.emplace_back(a);
    // systemd-run executes the command in place, so its failure would end this process: try it first.
    auto probe = run;
    probe.emplace_back("true");
    if (!Succeeds(probe)) return;
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return;
    run.push_back(self.string());
    for (int i = 1; argv[i] != nullptr; ++i) run.emplace_back(argv[i]);
    std::vector<char*> cargv;
    for (auto& a : run) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    setenv(kScopedEnv, "1", 1);
    execvp(cargv[0], cargv.data());
    unsetenv(kScopedEnv); // it could not be executed: go on without
}

Work SetUpWork(uint64_t budget_bytes) {
    static std::mutex mu;
    static fs::path root;
    std::lock_guard lock(mu);
    Work w;
    if (root.empty()) {
        if (std::string why = ContainmentProblem(); !why.empty()) {
            w.problem = std::move(why);
            return w;
        }
        const fs::path dir = *Delegated();
        const fs::path leaf = dir / kDaemonCgroup;
        std::error_code ec;
        fs::create_directory(leaf, ec);
        if (SelfCgroup() != leaf) {
            if (std::string err = Write(leaf / "cgroup.procs", "0"); !err.empty()) {
                w.problem = "cannot move zmd into " + leaf.string() + ": " + err;
                return w;
            }
        }
        if (std::string err = EnableForChildren(dir, kRequired); !err.empty()) {
            w.problem = std::move(err);
            return w;
        }
        const fs::path work = dir / kWorkCgroup;
        fs::create_directory(work, ec);
        if (ec) {
            w.problem = "cannot create " + work.string() + ": " + ec.message();
            return w;
        }
        // Batches of a zmd that was killed: their processes died with it (--die-with-parent).
        for (const auto& e : fs::directory_iterator(work, ec)) {
            if (e.is_directory()) Remove(e.path());
        }
        if (std::string err = EnableForChildren(work, kRequired); !err.empty()) {
            w.problem = std::move(err);
            return w;
        }
        if (!fs::exists(work / "memory.peak", ec)) {
            w.problem = "this kernel cannot report a cgroup's peak memory (memory.peak needs Linux 5.19 or newer)";
            return w;
        }
        Write(work / "memory.swap.max", "0"); // absent without swap accounting: no swap to keep work off
        root = work;
    }
    if (std::string err = Write(root / "memory.max", budget_bytes > 0 ? std::to_string(budget_bytes) : "max");
        !err.empty()) {
        w.problem = "cannot set the memory budget: " + err;
        return w;
    }
    w.root = root;
    w.budget = budget_bytes;
    return w;
}

std::optional<fs::path> CreateBatch(const fs::path& work, int cores, uint64_t mem_limit, std::string* error) {
    const fs::path dir = work / ("b-" + RandomHex(8));
    std::error_code ec;
    if (!fs::create_directory(dir, ec)) {
        if (error) *error = "cannot create " + dir.string() + ": " + ec.message();
        return std::nullopt;
    }
    std::string err;
    if (mem_limit > 0) {
        err = Write(dir / "memory.max", std::to_string(mem_limit));
        if (err.empty()) Write(dir / "memory.swap.max", "0");
    }
    if (err.empty()) err = Write(dir / "memory.oom.group", "1");
    if (err.empty() && cores > 0) err = Write(dir / "cpu.max", std::to_string(static_cast<int64_t>(cores) * 100000) + " 100000");
    if (err.empty()) err = Write(dir / "pids.max", std::to_string(kBatchPids));
    if (err.empty()) err = EnableForChildren(dir, {"memory"});
    if (!err.empty()) {
        Remove(dir);
        if (error) *error = std::move(err);
        return std::nullopt;
    }
    return dir;
}

std::optional<fs::path> CreateChild(const fs::path& batch, const std::string& name, std::string* error) {
    const fs::path dir = batch / name;
    std::error_code ec;
    if (!fs::create_directory(dir, ec)) {
        if (error) *error = "cannot create " + dir.string() + ": " + (ec ? ec.message() : "it exists");
        return std::nullopt;
    }
    return dir;
}

bool Remove(const fs::path& dir, std::chrono::milliseconds wait) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return true;
    const fs::path events = dir / "cgroup.events";
    if (ReadKeyed(events, "populated").value_or(0) == 1) {
        KillAll(dir);
        // cgroup.events wakes poll(POLLPRI) when "populated" changes; the cap only rechecks.
        const int fd = open(events.c_str(), O_RDONLY | O_CLOEXEC);
        const auto deadline = Clock::now() + wait;
        while (fd >= 0 && ReadKeyed(events, "populated").value_or(0) == 1) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (left <= 0) break;
            pollfd p{fd, POLLPRI, 0};
            poll(&p, 1, static_cast<int>(std::min<int64_t>(left, 100)));
        }
        if (fd >= 0) close(fd);
    }
    std::vector<fs::path> dirs;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    std::sort(dirs.rbegin(), dirs.rend()); // a child sorts after its parent: reversed, children go first
    dirs.push_back(dir);
    bool ok = true;
    for (const auto& d : dirs) {
        if (rmdir(d.c_str()) != 0 && errno != ENOENT) ok = false;
    }
    return ok;
}

Reaper::Reaper() : thread_([this](std::stop_token st) { Loop(st); }) {}

Reaper::~Reaper() {
    thread_.request_stop();
    cv_.notify_all();
}

void Reaper::Remove(fs::path dir) {
    {
        std::lock_guard lock(mu_);
        queue_.push_back(std::move(dir));
    }
    cv_.notify_all();
}

void Reaper::Loop(std::stop_token st) {
    for (;;) {
        fs::path dir;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, st, [this] { return !queue_.empty(); });
            if (queue_.empty()) return; // stopping, and nothing left to remove
            dir = std::move(queue_.front());
            queue_.pop_front();
        }
        cgroup::Remove(dir);
    }
}

int OpenProcs(const fs::path& dir) {
    return open((dir / "cgroup.procs").c_str(), O_WRONLY | O_CLOEXEC);
}

uint64_t OomKills(const fs::path& dir) {
    return ReadKeyed(dir / "memory.events", "oom_kill").value_or(0);
}

uint64_t LimitOoms(const fs::path& dir) {
    return ReadKeyed(dir / "memory.events.local", "oom").value_or(0);
}

bool Frozen(const fs::path& dir) {
    return ReadKeyed(dir / "cgroup.events", "frozen").value_or(0) == 1;
}

bool SetFrozen(const fs::path& dir, bool frozen) {
    return Write(dir / "cgroup.freeze", frozen ? "1" : "0").empty();
}

std::optional<uint64_t> CpuUsageUsec(const fs::path& dir) {
    return ReadKeyed(dir / "cpu.stat", "usage_usec");
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
    return Write(dir / "cgroup.kill", "1").empty();
}

} // namespace zeromesh::exec::cgroup
