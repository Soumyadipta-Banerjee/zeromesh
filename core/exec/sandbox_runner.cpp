#include "exec/sandbox_runner.hpp"

#include "exec/bwrap.hpp"
#include "exec/priority.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sodium.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>

namespace zeromesh::exec {

namespace {

namespace fs = std::filesystem;

std::string RandomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    randombytes_buf(raw.data(), raw.size());
    std::ostringstream ss;
    for (unsigned char c : raw) ss << "0123456789abcdef"[c >> 4] << "0123456789abcdef"[c & 15];
    return ss.str();
}

std::string FindOnPath(const std::string& name) {
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        fs::path candidate = fs::path(dir) / name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
    return {};
}

// Relative, no "..", no leading '/'.
bool SafeRelative(const std::string& p) {
    if (p.empty() || p[0] == '/') return false;
    for (const auto& part : fs::path(p)) {
        if (part == "..") return false;
    }
    return true;
}

// True if any path component before the final one is a symlink (or missing / not a directory).
bool SymlinkAmongParents(const fs::path& root, const fs::path& rel) {
    fs::path cur = root;
    fs::path norm = rel.lexically_normal();
    if (norm.empty() || norm == ".") return false;

    auto it = norm.begin();
    const auto end = norm.end();
    for (; it != end; ++it) {
        if (*it == ".") continue;
        if (*it == "..") return true;
        cur /= *it;
        auto next = std::next(it);
        if (next == end) break;
        struct stat st{};
        if (lstat(cur.c_str(), &st) != 0) return true;
        if (S_ISLNK(st.st_mode)) return true;
        if (!S_ISDIR(st.st_mode)) return true;
    }
    return false;
}

// Output paths must stay under work; parent directories must not be symlinks (the leaf may be).
bool SafeOutputPath(const fs::path& work, const std::string& rel) {
    if (!SafeRelative(rel)) return false;
    fs::path norm = fs::path(rel).lexically_normal();
    const std::string norm_str = norm.string();
    if (norm_str.empty() || norm_str == ".") return false;
    if (SymlinkAmongParents(work, norm)) return false;

    const fs::path full = work / norm;
    struct stat st{};
    if (lstat(full.c_str(), &st) != 0) return true;

    if (S_ISLNK(st.st_mode)) return true;

    std::error_code ec;
    const fs::path canon_file = fs::weakly_canonical(full, ec);
    if (ec) return false;
    const fs::path canon_work = fs::weakly_canonical(work, ec);
    if (ec) return false;
    const fs::path inside = fs::relative(canon_file, canon_work, ec);
    if (ec) return false;
    if (inside.empty() || inside == ".") return true;
    const std::string rel_str = inside.string();
    return rel_str != ".." && rel_str.rfind("../", 0) != 0;
}

struct FileStamp {
    uintmax_t size;
    int64_t mtime_ns;
};

using Snapshot = std::map<std::string, FileStamp>;

Snapshot TakeSnapshot(const fs::path& root) {
    Snapshot snap;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        struct stat st{};
        if (lstat(it->path().c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) continue;
        const auto rel = fs::relative(it->path(), root);
        if (SymlinkAmongParents(root, rel)) continue;
        snap[rel.string()] =
            FileStamp{static_cast<uintmax_t>(st.st_size), st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec};
    }
    return snap;
}

// Makes everything owner-writable so remove_all succeeds even if the task chmod'ed things.
void ForceRemove(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_symlink(ec)) {
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        }
    }
    fs::remove_all(dir, ec);
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string ReadSmallFile(const fs::path& p) {
    std::ifstream in(p);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return Trim(ss.str());
}

bool WriteSmallFile(const fs::path& p, const std::string& data) {
    std::ofstream out(p);
    if (!out) return false;
    out << data;
    return static_cast<bool>(out);
}

bool ControllerListed(const std::string& haystack, const char* name) {
    std::string n(name);
    size_t pos = 0;
    while (pos < haystack.size()) {
        while (pos < haystack.size() && haystack[pos] == ' ') ++pos;
        size_t end = haystack.find(' ', pos);
        if (end == std::string::npos) end = haystack.size();
        if (haystack.compare(pos, end - pos, n) == 0) return true;
        pos = end + 1;
    }
    return false;
}

bool EnableSubtreeControllers(const fs::path& parent, const char* const* names, size_t count) {
    const std::string avail = ReadSmallFile(parent / "cgroup.controllers");
    const std::string enabled = ReadSmallFile(parent / "cgroup.subtree_control");
    std::ostringstream cmd;
    for (size_t i = 0; i < count; ++i) {
        if (!ControllerListed(avail, names[i])) continue;
        if (ControllerListed(enabled, names[i])) continue;
        cmd << '+' << names[i] << ' ';
    }
    const std::string add = cmd.str();
    if (add.empty()) return true;
    return WriteSmallFile(parent / "cgroup.subtree_control", add);
}

bool CgroupFileWritable(const fs::path& p) {
    return access(p.c_str(), W_OK) == 0;
}

void JoinCgroup(const fs::path& dir);

constexpr const char* kDaemonCgroupName = "zm-daemon";

std::optional<fs::path> SelfCgroupSysfs() {
    std::ifstream cg("/proc/self/cgroup");
    std::string line;
    std::string rel;
    while (std::getline(cg, line)) {
        if (line.rfind("0::", 0) == 0) {
            rel = line.substr(3);
            break;
        }
    }
    if (rel.empty() || rel[0] != '/') return std::nullopt;
    return fs::path("/sys/fs/cgroup") / rel.substr(1);
}

// Deepest cgroup along our ancestry where we can create child cgroups with limits.
// Does not touch cgroup.subtree_control on ancestors (systemd delegation breaks otherwise).
std::optional<fs::path> FindDelegatableCgroupParent() {
    auto cur = SelfCgroupSysfs();
    if (!cur) return std::nullopt;
    const fs::path root = "/sys/fs/cgroup";
    while (true) {
        const std::string probe_name = ".zm-cg-probe-" + RandomHex(4);
        const fs::path probe = *cur / probe_name;
        std::error_code ec;
        fs::create_directory(probe, ec);
        if (!ec) {
            const bool mem = CgroupFileWritable(probe / "memory.max");
            const bool cpu = CgroupFileWritable(probe / "cpu.max");
            fs::remove(probe, ec);
            if (mem || cpu) return *cur;
        }
        if (*cur == root || cur->parent_path() == *cur) break;
        cur = cur->parent_path();
    }
    return std::nullopt;
}

std::optional<fs::path> g_task_cgroup_parent;
std::once_flag g_task_cgroup_parent_once;

// Parent of zm-daemon and zm-task-*; daemon processes live in zm-daemon (leaf).
std::optional<fs::path> FindTaskCgroupParent() {
    std::call_once(g_task_cgroup_parent_once, [] {
        auto parent = FindDelegatableCgroupParent();
        if (!parent) return;

        static const char* kCtrls[] = {"memory", "cpu", "pids", "io"};
        if (!EnableSubtreeControllers(*parent, kCtrls, 4)) return;

        const fs::path daemon_cg = *parent / kDaemonCgroupName;
        std::error_code ec;
        fs::create_directory(daemon_cg, ec);
        if (ec) return;

        JoinCgroup(daemon_cg);
        g_task_cgroup_parent = *parent;
    });
    return g_task_cgroup_parent;
}

struct PreparedCgroup {
    fs::path dir;
    bool memory{false};
    bool cpu{false};
};

std::optional<PreparedCgroup> PrepareTaskCgroup(const fs::path& parent, int cores, uint64_t mem_bytes) {
    static const char* kCtrls[] = {"memory", "cpu", "pids", "io"};
    if (!EnableSubtreeControllers(parent, kCtrls, 4)) return std::nullopt;

    PreparedCgroup out;
    out.dir = parent / ("zm-task-" + RandomHex(8));
    std::error_code ec;
    fs::create_directory(out.dir, ec);
    if (ec) return std::nullopt;

    if (mem_bytes > 0 && CgroupFileWritable(out.dir / "memory.max")) {
        if (!WriteSmallFile(out.dir / "memory.max", std::to_string(mem_bytes))) {
            fs::remove(out.dir, ec);
            return std::nullopt;
        }
        if (CgroupFileWritable(out.dir / "memory.swap.max")) WriteSmallFile(out.dir / "memory.swap.max", "0");
        out.memory = true;
    }
    if (cores > 0 && CgroupFileWritable(out.dir / "cpu.max")) {
        const std::string quota = std::to_string(static_cast<int64_t>(cores) * 100000) + " 100000";
        if (WriteSmallFile(out.dir / "cpu.max", quota)) out.cpu = true;
    }
    if (CgroupFileWritable(out.dir / "pids.max")) WriteSmallFile(out.dir / "pids.max", "4096");

    if (!out.memory && !out.cpu) {
        fs::remove(out.dir, ec);
        return std::nullopt;
    }
    return out;
}

void JoinCgroup(const fs::path& dir) {
    const fs::path procs = dir / "cgroup.procs";
    int fd = open(procs.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    const std::string pid = std::to_string(getpid()) + "\n";
    (void)!write(fd, pid.data(), pid.size());
    close(fd);
}

void RemoveTaskCgroup(const fs::path& dir) {
    std::error_code ec;
    fs::remove(dir, ec);
}

// RLIMIT_DATA caps the heap; it does not limit mmap-heavy workloads as tightly as cgroup memory.max.
void ApplyRlimitMemory(uint64_t mem_bytes) {
    if (mem_bytes == 0) return;
    rlimit rl{};
    const rlim_t cap = static_cast<rlim_t>(mem_bytes);
    rl.rlim_cur = rl.rlim_max = cap;
    setrlimit(RLIMIT_DATA, &rl);
}

void ApplyBaselineRlimits() {
    rlimit rc{};
    rc.rlim_cur = rc.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &rc);
}

bool CgroupHadOomKill(const fs::path& dir) {
    const std::string events = ReadSmallFile(dir / "memory.events");
    const std::string key = "oom_kill ";
    const auto pos = events.find(key);
    if (pos == std::string::npos) return false;
    size_t i = pos + key.size();
    while (i < events.size() && events[i] == ' ') ++i;
    long long count = 0;
    try {
        count = std::stoll(events.substr(i));
    } catch (...) {
        return false;
    }
    return count > 0;
}

} // namespace

ResourceEnforcementStatus ProbeSandboxResourceEnforcement() {
    ResourceEnforcementStatus st;
    st.rlimit_memory = true;
    auto parent = FindTaskCgroupParent();
    if (!parent) return st;
    auto cg = PrepareTaskCgroup(*parent, 1, 64 * 1024);
    if (!cg) return st;
    st.cgroup_memory = cg->memory;
    st.cgroup_cpu = cg->cpu;
    RemoveTaskCgroup(cg->dir);
    return st;
}

std::optional<fs::path> DelegatedCgroupParent() {
    return FindDelegatableCgroupParent();
}

void SandboxRunner::SetCgroupObserver(CgroupObserver observer) {
    cgroup_observer_ = std::move(observer);
}

SandboxRunner::SandboxRunner(std::shared_ptr<store::CasStore> store, std::shared_ptr<store::MerkleBuilder> merkle,
                             fs::path scratch_base)
    : store_(std::move(store)), merkle_(std::move(merkle)), scratch_base_(std::move(scratch_base)) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium init failed");
    fs::create_directories(scratch_base_);
    bwrap_path_ = FindOnPath("bwrap");
    if (!bwrap_path_.empty()) {
        bwrap_error_ = CheckBwrapBinary(bwrap_path_);
        if (!bwrap_error_.empty()) bwrap_path_.clear();
    }
}

RunResult SandboxRunner::Run(const RunSpec& spec, const LogSink& sink, std::stop_token stop) {
    auto start = std::chrono::steady_clock::now();
    RunResult result;

    if (spec.argv.empty()) {
        result.error = "empty command";
        return result;
    }
    if (bwrap_path_.empty()) {
        result.error = bwrap_error_.empty() ? "bubblewrap (bwrap) is not installed on this node" : bwrap_error_;
        return result;
    }
    if (!SafeRelative(spec.working_dir.empty() ? "." : spec.working_dir)) {
        result.error = "working_dir must be relative to /work";
        return result;
    }

    const fs::path scratch = scratch_base_ / RandomHex(8);
    const fs::path work = scratch / "work";
    fs::create_directories(work);

    if (!spec.input_root.hash.empty() && !merkle_->MaterializeTree(spec.input_root, work)) {
        result.error = "failed to materialize inputs";
        ForceRemove(scratch);
        return result;
    }
    fs::create_directories(work / spec.working_dir);
    // REAPI/compilers often write outputs under working_dir; ensure parents exist.
    for (const auto& out : spec.outputs) {
        if (!SafeRelative(out)) continue;
        std::error_code ec;
        fs::create_directories((work / spec.working_dir / out).parent_path(), ec);
    }

    Snapshot before;
    if (spec.capture_changed) before = TakeSnapshot(work);

    BwrapBuilder b;
    b.SetBinary(bwrap_path_).ShareNetwork(spec.allow_network).AddDev("/dev").AddProc("/proc");
    // Binds before symlinks so parents exist (/usr before /bin→usr/bin, /etc binds before /etc/mtab).
    for (const auto& m : spec.sandbox_env.binds) {
        if (m.read_only)
            b.AddReadOnlyBind(m.host, m.sandbox);
        else
            b.AddReadWriteBind(m.host, m.sandbox);
    }
    for (const auto& s : spec.sandbox_env.symlinks) b.AddSymlink(s.target, s.link);
    b.AddTmpfs("/tmp").AddReadWriteBind(work.string(), "/work");
    // GLib/Wayland require XDG_RUNTIME_DIR mode 0700. Plain /tmp is not valid, and
    // older Blender (e.g. Debian) aborts headless with only a mount-list warning first.
    b.AddPerms(0700).AddDir("/tmp/xdg-runtime");
    b.AddEnv("HOME", "/tmp").AddEnv("ZM_NODE_ID", spec.node_id);
    // Headless: older Blender still opens GHOST/Wayland and dies if these look like a GUI.
    b.AddEnv("SDL_VIDEODRIVER", "dummy");
    b.AddEnv("LIBGL_ALWAYS_SOFTWARE", "1");
    for (const auto& [k, v] : spec.sandbox_env.env) b.AddEnv(k, v);
    for (const auto& [k, v] : spec.env) b.AddEnv(k, v);
    // Force a valid runtime dir last so task env cannot point at /tmp (mode ≠ 0700).
    b.AddEnv("XDG_RUNTIME_DIR", "/tmp/xdg-runtime");
    b.AddEnv("WAYLAND_DISPLAY", "");
    b.AddEnv("DISPLAY", "");
    b.SetWorkingDirectory((fs::path("/work") / spec.working_dir).lexically_normal().string());
    auto args = b.Build(spec.argv);

    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0) {
        result.error = "pipe failed";
        ForceRemove(scratch);
        return result;
    }

    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);

    std::optional<PreparedCgroup> task_cgroup;
    const bool want_mem = spec.mem_bytes > 0;
    const bool want_cpu = spec.cores > 0;
    if (want_mem || want_cpu) {
        if (auto parent = FindTaskCgroupParent()) task_cgroup = PrepareTaskCgroup(*parent, spec.cores, spec.mem_bytes);
    }
    if (task_cgroup && cgroup_observer_) cgroup_observer_(task_cgroup->dir, true);
    if (task_cgroup) {
        result.memory_enforced = task_cgroup->memory;
        result.cpu_enforced = task_cgroup->cpu;
    }
    const bool use_rlimit_mem = want_mem && !(task_cgroup && task_cgroup->memory);
    if (use_rlimit_mem) result.memory_enforced = true;
    if (want_cpu && !(task_cgroup && task_cgroup->cpu)) {
        result.resource_note = "CPU limit not enforced without cgroup delegation";
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        if (task_cgroup) RemoveTaskCgroup(task_cgroup->dir);
        ForceRemove(scratch);
        return result;
    }
    if (pid == 0) {
        if (task_cgroup) JoinCgroup(task_cgroup->dir);
        if (use_rlimit_mem) ApplyRlimitMemory(spec.mem_bytes);
        ApplyBaselineRlimits();
        setpgid(0, 0);
        EnterIdlePriority();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        execv(cargv[0], cargv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(out_pipe[1]);
    close(err_pipe[1]);

    std::ofstream out_file(scratch / "stdout", std::ios::binary);
    std::ofstream err_file(scratch / "stderr", std::ios::binary);
    pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    int open_fds = 2;
    char buf[65536];
    const auto deadline = start + spec.timeout;
    bool killed = false;

    while (open_fds > 0) {
        if (!killed && (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)) {
            if (stop.stop_requested())
                result.cancelled = true;
            else
                result.timed_out = true;
            kill(-pid, SIGKILL);
            killed = true;
        }
        int r = poll(fds, 2, 100);
        if (r <= 0) continue;
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(fds[i].fd, buf, sizeof(buf));
            if (n > 0) {
                std::string_view chunk(buf, static_cast<size_t>(n));
                std::string& head = i == 0 ? result.stdout_head : result.stderr_head;
                if (head.size() < kLogHeadBytes) head.append(chunk.substr(0, kLogHeadBytes - head.size()));
                (i == 0 ? out_file : err_file).write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                if (sink) sink(i + 1, chunk);
            } else {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open_fds;
            }
        }
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);
    // bwrap reports a missing command as exit 1 + "bwrap: execvp ..."; use the shell convention.
    if (result.exit_code == 1 && result.stderr_head.rfind("bwrap: execvp ", 0) == 0) result.exit_code = 127;

    if (!result.timed_out && !result.cancelled && result.memory_enforced && task_cgroup &&
        CgroupHadOomKill(task_cgroup->dir)) {
        result.error = "task exceeded memory limit (killed)";
    }
    if (task_cgroup) {
        if (cgroup_observer_) cgroup_observer_(task_cgroup->dir, false);
        RemoveTaskCgroup(task_cgroup->dir);
    }

    out_file.close();
    err_file.close();
    result.stdout_digest = store_->PutBlobFile(scratch / "stdout");
    result.stderr_digest = store_->PutBlobFile(scratch / "stderr");

    auto capture = [&](const std::string& rel) {
        // Prefer work/working_dir/rel (REAPI compilers), fall back to work/rel.
        const std::string under_wd = (fs::path(spec.working_dir) / rel).lexically_normal().string();
        const std::string under_work = fs::path(rel).lexically_normal().string();
        std::error_code ec;
        std::string use = under_work;
        if (fs::exists(work / under_wd, ec))
            use = under_wd;
        else if (!fs::exists(work / under_work, ec) && SafeOutputPath(work, under_wd))
            use = under_wd;
        if (!SafeOutputPath(work, use)) return;
        fs::path p = work / fs::path(use).lexically_normal();
        struct stat st{};
        if (lstat(p.c_str(), &st) != 0) return;
        OutputEntry e;
        e.path = rel;
        if (S_ISLNK(st.st_mode)) {
            e.is_symlink = true;
            e.symlink_target = fs::read_symlink(p).string();
        } else if (S_ISDIR(st.st_mode)) {
            e.is_dir = true;
            e.digest = merkle_->BuildTree(p);
        } else if (S_ISREG(st.st_mode)) {
            e.digest = store_->PutBlobFile(p);
            e.is_executable = (st.st_mode & S_IXUSR) != 0;
        } else {
            return;
        }
        result.outputs.push_back(std::move(e));
    };

    if (!result.timed_out && !result.cancelled) {
        std::map<std::string, bool> seen;
        for (const auto& o : spec.outputs) {
            if (!SafeRelative(o)) continue;
            std::string rel = fs::path(o).lexically_normal().string();
            if (seen.emplace(rel, true).second) capture(rel);
        }
        if (spec.capture_changed) {
            for (const auto& [rel, stamp] : TakeSnapshot(work)) {
                auto it = before.find(rel);
                bool changed =
                    it == before.end() || it->second.size != stamp.size || it->second.mtime_ns != stamp.mtime_ns;
                if (changed && seen.emplace(rel, true).second) capture(rel);
            }
        }
    }

    ForceRemove(scratch);
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return result;
}

} // namespace zeromesh::exec
