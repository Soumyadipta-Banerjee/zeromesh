#include "exec/batch_runner.hpp"

#include "exec/bwrap.hpp"
#include "exec/cgroup.hpp"
#include "exec/depfile.hpp"
#include "exec/priority.hpp"
#include "exec/subprocess.hpp"
#include "exec/tree_watch.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sodium.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace zeromesh::exec {

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr auto kTick = std::chrono::milliseconds(200);
constexpr size_t kMaxIdBytes = 200;

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

// Relative, no "..", no leading '/'.
bool SafeRelative(const std::string& p) {
    if (p.empty() || p[0] == '/' || p.find('\0') != std::string::npos) return false;
    for (const auto& part : fs::path(p)) {
        if (part == "..") return false;
    }
    return true;
}

// `rel` under `cwd`, normalized, '/'-separated; "." for the root itself.
std::string JoinRel(const std::string& cwd, const std::string& rel) {
    fs::path p = (fs::path(cwd) / rel).lexically_normal();
    std::string s = p.string();
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s.empty() ? "." : s;
}

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > kMaxIdBytes) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; });
}

// True if a component of `rel` before the last is a symlink or not a directory under `root`.
bool SymlinkAmongParents(const fs::path& root, const std::string& rel) {
    fs::path cur = root;
    const fs::path norm = fs::path(rel).lexically_normal();
    for (auto it = norm.begin(); it != norm.end(); ++it) {
        if (*it == "." || it->empty()) continue;
        if (*it == "..") return true;
        if (std::next(it) == norm.end()) break;
        cur /= *it;
        struct stat st{};
        if (lstat(cur.c_str(), &st) != 0) return false; // missing: nothing to escape through
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) return true;
    }
    return false;
}

void ForceRemove(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) {
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        }
    }
    fs::remove_all(dir, ec);
}

// An overlayfs whiteout: the item deleted a file of the lower layer.
bool IsWhiteout(const struct stat& st) {
    return S_ISCHR(st.st_mode) && st.st_rdev == 0;
}

bool IsOpaqueDir(const fs::path& p) {
    char v = 0;
    for (const char* name : {"user.overlay.opaque", "trusted.overlay.opaque"}) {
        if (lgetxattr(p.c_str(), name, &v, 1) == 1 && v == 'y') return true;
    }
    return false;
}

struct Stamp {
    int64_t size{0};
    int64_t mtime_ns{0};
};

std::map<std::string, Stamp> Stamps(const fs::path& root) {
    std::map<std::string, Stamp> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        struct stat st{};
        if (lstat(it->path().c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) continue;
        out[fs::relative(it->path(), root, ec).string()] = {static_cast<int64_t>(st.st_size),
                                                            st.st_mtim.tv_sec * 1'000'000'000LL + st.st_mtim.tv_nsec};
    }
    return out;
}

void AppendTail(std::string& tail, std::string_view chunk) {
    tail.append(chunk);
    if (tail.size() > kLogTailBytes) tail.erase(0, tail.size() - kLogTailBytes);
}

std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size())) {
        s.replace(pos, from.size(), to);
    }
    return s;
}

// Every process descended from `root` (inclusive), from /proc.
std::vector<pid_t> Descendants(pid_t root) {
    std::unordered_map<pid_t, std::vector<pid_t>> children;
    std::error_code ec;
    for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;
        std::ifstream stat(it->path() / "stat");
        std::string line;
        if (!std::getline(stat, line)) continue;
        const size_t close = line.rfind(')');
        if (close == std::string::npos) continue;
        std::istringstream rest(line.substr(close + 2));
        char state = 0;
        pid_t ppid = 0;
        rest >> state >> ppid;
        children[ppid].push_back(static_cast<pid_t>(std::stol(name)));
    }
    std::vector<pid_t> out{root};
    for (size_t i = 0; i < out.size(); ++i) {
        for (pid_t c : children[out[i]]) out.push_back(c);
    }
    return out;
}

// User+system CPU of the processes in `pids`, in milliseconds.
int64_t CpuMsOf(const std::vector<pid_t>& pids) {
    const long ticks = sysconf(_SC_CLK_TCK);
    int64_t total = 0;
    for (pid_t p : pids) {
        std::ifstream stat("/proc/" + std::to_string(p) + "/stat");
        std::string line;
        if (!std::getline(stat, line)) continue;
        const size_t close = line.rfind(')');
        if (close == std::string::npos) continue;
        std::istringstream rest(line.substr(close + 2));
        std::string field;
        int64_t utime = 0, stime = 0;
        for (int i = 3; i <= 15 && rest >> field; ++i) {
            if (i == 14) utime = std::stoll(field);
            if (i == 15) stime = std::stoll(field);
        }
        total += utime + stime;
    }
    return ticks > 0 ? total * 1000 / ticks : 0;
}

std::string Exe(pid_t p) {
    std::error_code ec;
    return fs::read_symlink("/proc/" + std::to_string(p) + "/exe", ec).string();
}

const char* FailureWord(ItemFailure f) {
    switch (f) {
    case ItemFailure::None:
        return "ok";
    case ItemFailure::Exit:
        return "exit";
    case ItemFailure::MissingOutput:
        return "missing output";
    case ItemFailure::Oom:
        return "out of memory";
    case ItemFailure::Timeout:
        return "timeout";
    case ItemFailure::Hung:
        return "hung";
    case ItemFailure::Sandbox:
        return "sandbox";
    }
    return "?";
}

} // namespace

struct BatchRunner::Group {
    std::vector<const ItemSpec*> items;
    bool combined{false};
    fs::path dir;
};

BatchRunner::BatchRunner(std::shared_ptr<store::CasStore> store, std::shared_ptr<store::MerkleBuilder> merkle,
                         fs::path scratch_base)
    : store_(std::move(store)), merkle_(std::move(merkle)), scratch_base_(std::move(scratch_base)),
      inputs_(scratch_base_ / "inputs", merkle_) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium init failed");
    fs::create_directories(scratch_base_);
    bwrap_path_ = FindOnPath("bwrap");
    if (bwrap_path_.empty()) {
        bwrap_error_ = "bubblewrap (bwrap) is not installed on this node";
    } else {
        bwrap_error_ = CheckBwrapBinary(bwrap_path_);
    }
    if (bwrap_error_.empty()) overlay_ = ProbeOverlay();
    if (!bwrap_error_.empty()) bwrap_path_.clear();
}

// Runs bwrap twice: once plain (does it work at all here? AppArmor's userns restriction breaks
// it), once with an overlay at a tmpfs path (are unprivileged overlays allowed?).
bool BatchRunner::ProbeOverlay() {
    auto run = [](const std::vector<std::string>& args, std::string* err) {
        CommandResult r = RunCommand(args, {.cwd = {}, .timeout = std::chrono::seconds(30), .max_output = 4096});
        std::string out = r.error.empty() ? r.output : r.error;
        while (!out.empty() && out.back() == '\n') out.pop_back();
        if (err) *err = out;
        return r.ok();
    };
    const std::string t = fs::exists("/bin/true") ? "/bin/true" : "/usr/bin/true";
    std::string err;
    if (!run({bwrap_path_, "--unshare-all", "--die-with-parent", "--ro-bind", "/", "/", "--", t}, &err)) {
        bwrap_error_ = "bubblewrap cannot create a sandbox here" + (err.empty() ? std::string() : ": " + err);
        return false;
    }
    const fs::path probe = scratch_base_ / ("overlay-probe-" + RandomHex(4));
    std::error_code ec;
    for (const char* d : {"lower", "upper", "work"}) fs::create_directories(probe / d, ec);
    const bool ok = run({bwrap_path_, "--unshare-all", "--die-with-parent", "--ro-bind", "/", "/", "--tmpfs", "/tmp",
                         "--overlay-src", (probe / "lower").string(), "--overlay", (probe / "upper").string(),
                         (probe / "work").string(), "/tmp/o", "--", t},
                        nullptr);
    ForceRemove(probe);
    return ok;
}

void BatchRunner::SetCgroupObserver(CgroupObserver observer) {
    cgroup_observer_ = std::move(observer);
}

std::optional<fs::path> BatchRunner::CgroupFor(const std::string& tag) const {
    std::lock_guard lock(tagged_mu_);
    auto it = tagged_.find(tag);
    if (it == tagged_.end()) return std::nullopt;
    return it->second;
}

bool BatchRunner::SetFrozen(const std::string& tag, bool frozen) {
    auto dir = CgroupFor(tag);
    return dir && cgroup::SetFrozen(*dir, frozen);
}

void BatchRunner::PruneInputs(std::chrono::seconds idle) {
    inputs_.Prune(idle);
}

BatchRunResult BatchRunner::Run(const BatchRunSpec& spec, const BatchCallbacks& cb, std::stop_token stop) {
    BatchRunResult result;
    if (!bwrap_error_.empty()) {
        result.error = bwrap_error_;
        return result;
    }
    if (spec.items.empty()) {
        result.error = "empty batch";
        return result;
    }
    std::set<std::string> ids;
    for (const auto& it : spec.items) {
        if (!ValidId(it.id) || !ids.insert(it.id).second) {
            result.error = "invalid or duplicate item id '" + it.id + "'";
            return result;
        }
        if (it.argv.empty() && spec.combine_argv.empty()) {
            result.error = "item '" + it.id + "' has an empty command";
            return result;
        }
        if (!SafeRelative(it.cwd.empty() ? "." : it.cwd)) {
            result.error = "item '" + it.id + "': cwd must be relative to /work";
            return result;
        }
        for (const auto* list : {&it.outputs, &it.optional_outputs}) {
            for (const auto& o : *list) {
                if (!SafeRelative(o)) {
                    result.error = "item '" + it.id + "': unsafe output path '" + o + "'";
                    return result;
                }
            }
        }
        if (!it.depfile.empty() && !SafeRelative(it.depfile)) {
            result.error = "item '" + it.id + "': unsafe depfile path '" + it.depfile + "'";
            return result;
        }
    }

    auto lease = inputs_.Acquire(spec.input_root);
    if (!lease) {
        result.error = "input tree " + spec.input_root.hash + " is incomplete in this node's store";
        return result;
    }

    const fs::path scratch = scratch_base_ / ("b-" + RandomHex(8));
    fs::create_directories(scratch);

    std::optional<cgroup::Group> group_cg;
    if (auto parent = cgroup::TaskParent()) group_cg = cgroup::Create(*parent, spec.cores, spec.mem_bytes);
    if (group_cg) {
        if (cgroup_observer_) cgroup_observer_(group_cg->dir, true);
        if (!spec.tag.empty()) {
            std::lock_guard lock(tagged_mu_);
            tagged_[spec.tag] = group_cg->dir;
        }
        result.memory_enforced = group_cg->memory;
        result.cpu_enforced = group_cg->cpu;
    }

    std::vector<Group> groups;
    if (!spec.combine_argv.empty()) {
        Group g;
        g.combined = true;
        for (const auto& it : spec.items) g.items.push_back(&it);
        groups.push_back(std::move(g));
    } else {
        for (const auto& it : spec.items) {
            Group g;
            g.items.push_back(&it);
            groups.push_back(std::move(g));
        }
    }

    bool cancelled = false;
    for (size_t i = 0; i < groups.size() && !cancelled; ++i) {
        if (stop.stop_requested()) {
            cancelled = true;
            break;
        }
        groups[i].dir = scratch / ("g" + std::to_string(i));
        RunGroup(spec, groups[i], lease->path(), group_cg ? group_cg->dir : fs::path(), cb, stop, cancelled);
        ForceRemove(groups[i].dir);
    }
    result.cancelled = cancelled;

    if (group_cg) {
        if (!spec.tag.empty()) {
            std::lock_guard lock(tagged_mu_);
            tagged_.erase(spec.tag);
        }
        if (cgroup_observer_) cgroup_observer_(group_cg->dir, false);
        cgroup::Remove(group_cg->dir);
    }
    ForceRemove(scratch);
    return result;
}

void BatchRunner::RunGroup(const BatchRunSpec& spec, Group& g, const fs::path& lower, const fs::path& cg,
                           const BatchCallbacks& cb, std::stop_token stop, bool& cancelled) {
    const ItemSpec& first = *g.items.front();
    const std::string cwd = JoinRel(first.cwd.empty() ? "." : first.cwd, ".");
    const fs::path upper = g.dir / "upper";
    const fs::path ovwork = g.dir / "ovwork";
    const fs::path copy = g.dir / "work";
    const fs::path ckpt = g.dir / "checkpoint";
    std::error_code ec;
    fs::create_directories(ckpt, ec);

    auto fail_all = [&](ItemFailure kind, const std::string& msg) {
        for (const ItemSpec* it : g.items) {
            ItemRunResult r;
            r.id = it->id;
            r.failure = kind;
            r.message = msg;
            if (cb.on_item) cb.on_item(std::move(r));
        }
    };

    // The writable layer: the overlay's upper directory, or a private copy of the inputs.
    fs::path writable;
    if (overlay_) {
        fs::create_directories(upper, ec);
        fs::create_directories(ovwork, ec);
        writable = upper;
    } else {
        fs::create_directories(copy, ec);
        if (!spec.input_root.hash.empty() &&
            !merkle_->MaterializeTree(spec.input_root, copy, store::Materialize::Copy)) {
            fail_all(ItemFailure::Sandbox, "failed to copy the input tree");
            return;
        }
        writable = copy;
    }
    // Create the working directory and the parents of every declared output up front, so the
    // item can write there and a combined batch's watcher sees its outputs land.
    auto precreate = [&](const std::string& ws_rel) {
        if (ws_rel != "." && !SymlinkAmongParents(writable, ws_rel + "/x"))
            fs::create_directories(writable / ws_rel, ec);
    };
    precreate(cwd);
    for (const ItemSpec* it : g.items) {
        for (const auto* list : {&it->outputs, &it->optional_outputs}) {
            for (const auto& o : *list) precreate(fs::path(JoinRel(cwd, o)).parent_path().string());
        }
    }
    if (!first.checkpoint.hash.empty() && !merkle_->MaterializeTree(first.checkpoint, ckpt, store::Materialize::Copy)) {
        // A lost checkpoint costs work, not correctness: start from nothing.
        ForceRemove(ckpt);
        fs::create_directories(ckpt, ec);
    }
    std::map<std::string, Stamp> before;
    const bool want_changed =
        std::any_of(g.items.begin(), g.items.end(), [](const ItemSpec* it) { return it->capture_changed; });
    if (!overlay_ && want_changed) before = Stamps(copy / cwd);

    // ---- the command line ----
    BwrapBuilder b;
    b.SetBinary(bwrap_path_).ShareNetwork(spec.network).AddDev("/dev").AddProc("/proc");
    for (const auto& m : spec.sandbox_env.binds) {
        if (m.read_only)
            b.AddReadOnlyBind(m.host, m.sandbox);
        else
            b.AddReadWriteBind(m.host, m.sandbox);
    }
    for (const auto& s : spec.sandbox_env.symlinks) b.AddSymlink(s.target, s.link);
    b.AddTmpfs("/tmp");
    if (overlay_)
        b.AddOverlay(lower.string(), upper.string(), ovwork.string(), "/work");
    else
        b.AddReadWriteBind(copy.string(), "/work");
    b.AddReadWriteBind(ckpt.string(), "/checkpoint");
    if (!spec.warm_dir.empty()) {
        fs::create_directories(spec.warm_dir, ec);
        b.AddReadWriteBind(spec.warm_dir.string(), "/warm");
    }
    for (const auto& dev : spec.gpu_devices) {
        if (fs::exists(dev, ec)) b.AddDevBind(dev, dev);
    }
    // GLib/Wayland require XDG_RUNTIME_DIR to be mode 0700, which /tmp is not.
    b.AddPerms(0700).AddDir("/tmp/xdg-runtime");
    b.AddEnv("HOME", "/tmp").AddEnv("ZM_NODE_ID", spec.node_id);
    b.AddEnv("SDL_VIDEODRIVER", "dummy");
    if (spec.gpu_devices.empty()) b.AddEnv("LIBGL_ALWAYS_SOFTWARE", "1");
    for (const auto& [k, v] : spec.sandbox_env.env) b.AddEnv(k, v);
    for (const auto& [k, v] : spec.env) b.AddEnv(k, v);
    if (!g.combined) {
        for (const auto& [k, v] : first.env) b.AddEnv(k, v);
    }
    std::string ids;
    for (const ItemSpec* it : g.items) ids += (ids.empty() ? "" : " ") + it->id;
    b.AddEnv("XDG_RUNTIME_DIR", "/tmp/xdg-runtime").AddEnv("WAYLAND_DISPLAY", "").AddEnv("DISPLAY", "");
    b.AddEnv(g.combined ? "ZM_ITEMS" : "ZM_ITEM_ID", ids);
    b.AddEnv("ZM_CORES", std::to_string(std::max(spec.cores, 1)));
    b.AddEnv("ZM_MEM_BYTES", std::to_string(spec.mem_bytes));
    b.AddEnv("ZM_GPUS", std::to_string(spec.gpu_devices.empty() ? 0 : 1));
    b.AddEnv("ZM_CHECKPOINT_DIR", "/checkpoint");
    if (!spec.warm_dir.empty()) b.AddEnv("ZM_WARM_DIR", "/warm");
    b.SetWorkingDirectory((fs::path("/work") / cwd).lexically_normal().string());
    std::vector<std::string> command = first.argv;
    if (g.combined) {
        command.clear();
        for (const auto& a : spec.combine_argv) {
            command.push_back(ReplaceAll(ReplaceAll(a, "{first}", std::to_string(g.items.front()->seq)), "{last}",
                                         std::to_string(g.items.back()->seq)));
        }
    }
    auto args = b.Build(command);

    // ---- start ----
    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        fail_all(ItemFailure::Sandbox, "pipe failed");
        return;
    }
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        fail_all(ItemFailure::Sandbox, "pipe failed");
        return;
    }
    std::vector<char*> cargv;
    for (auto& a : args) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    const uint64_t oom_before = cg.empty() ? 0 : cgroup::OomKills(cg);
    const auto start = Clock::now();
    pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) close(fd);
        fail_all(ItemFailure::Sandbox, "fork failed");
        return;
    }
    if (pid == 0) {
        if (!cg.empty()) cgroup::Join(cg);
        rlimit core{};
        setrlimit(RLIMIT_CORE, &core);
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
    for (const ItemSpec* it : g.items) {
        if (cb.on_started) cb.on_started(it->id);
    }

    // ---- watch it ----
    std::ofstream out_file(g.dir / "stdout", std::ios::binary);
    std::ofstream err_file(g.dir / "stderr", std::ios::binary);
    std::string out_tail, err_tail;
    pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    int open_fds = 2;
    char buf[65536];
    uint64_t output_bytes = 0;

    auto deadline = start + spec.timeout * static_cast<int64_t>(g.items.size());
    auto last_progress = start;
    uint64_t last_output_bytes = 0;
    int64_t last_cpu = -1;
    auto last_tick = start;
    bool exited = false;
    auto exited_at = start;
    bool timed_out = false, hung = false, terminating = false, killed = false;
    auto term_started = start;

    std::string last_ckpt_hash;
    std::map<std::string, Stamp> ckpt_stamps;
    store::StatCache ckpt_hashes({});
    auto scan_checkpoint = [&] {
        auto now_stamps = Stamps(ckpt);
        std::erase_if(now_stamps,
                      [](const auto& kv) { return fs::path(kv.first).filename().string().starts_with("."); });
        if (now_stamps.size() == ckpt_stamps.size() &&
            std::equal(now_stamps.begin(), now_stamps.end(), ckpt_stamps.begin(), [](const auto& a, const auto& b) {
                return a.first == b.first && a.second.size == b.second.size && a.second.mtime_ns == b.second.mtime_ns;
            })) {
            return;
        }
        ckpt_stamps = std::move(now_stamps);
        // Dot-names are the staging area of an atomic write: never store a half-written file.
        const auto staging = store::IgnoreRules::FromLines({".*"});
        store::TreeOptions opts;
        opts.ignore = &staging;
        opts.stat_cache = &ckpt_hashes;
        try {
            const store::Digest tree = merkle_->BuildTree(ckpt, opts);
            if (!tree.hash.empty() && tree.hash != last_ckpt_hash && !ckpt_stamps.empty()) {
                last_ckpt_hash = tree.hash;
                if (cb.on_checkpoint) cb.on_checkpoint(first.id, tree);
            }
        } catch (const std::exception&) {
            // an unrepresentable name: the checkpoint is skipped, the item is not failed
        }
    };

    auto task_pids = [&] {
        std::vector<pid_t> pids = cg.empty() ? Descendants(pid) : cgroup::Procs(cg);
        std::erase_if(pids, [&](pid_t p) { return Exe(p) == bwrap_path_; });
        return pids;
    };
    auto cpu_ms_now = [&]() -> int64_t {
        if (!cg.empty()) {
            if (auto u = cgroup::CpuUsageUsec(cg)) return static_cast<int64_t>(*u / 1000);
        }
        return CpuMsOf(Descendants(pid));
    };
    auto begin_termination = [&] {
        if (terminating) return;
        terminating = true;
        term_started = Clock::now();
        for (pid_t p : task_pids()) kill(p, SIGTERM);
    };
    auto hard_kill = [&] {
        if (killed) return;
        killed = true;
        if (!cg.empty()) cgroup::KillAll(cg);
        kill(-pid, SIGKILL);
    };

    // Combined batches: commit each item once all its outputs are finished.
    std::unique_ptr<TreeWatch> watch;
    std::unordered_set<std::string> finished_paths;
    std::set<std::string> committed;
    if (g.combined) watch = std::make_unique<TreeWatch>(writable);
    int64_t cpu_at_last_commit = 0;
    auto commit_ready = [&](auto&& capture) {
        for (const ItemSpec* it : g.items) {
            if (committed.contains(it->id) || it->outputs.empty()) continue;
            bool all = true;
            for (const auto& o : it->outputs) all = all && finished_paths.contains(JoinRel(cwd, o));
            if (!all) continue;
            ItemRunResult r = capture(*it);
            if (r.failure != ItemFailure::None) continue; // a vanished output: decided at the end
            committed.insert(it->id);
            r.exit_code = 0;
            r.wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            const int64_t cpu = cpu_ms_now();
            r.cpu_ms = std::max<int64_t>(0, cpu - cpu_at_last_commit);
            cpu_at_last_commit = cpu;
            if (cb.on_item) cb.on_item(std::move(r));
        }
    };

    // ---- outputs ----
    // Where an item's path can be read after (or while) it runs: what it wrote, else the inputs.
    auto locate = [&](const std::string& ws_rel, fs::path* path, bool* in_upper) -> bool {
        if (overlay_) {
            if (!SymlinkAmongParents(upper, ws_rel)) {
                struct stat st{};
                if (lstat((upper / ws_rel).c_str(), &st) == 0) {
                    if (IsWhiteout(st)) return false;
                    *path = upper / ws_rel;
                    *in_upper = true;
                    return true;
                }
            }
            struct stat st{};
            if (!SymlinkAmongParents(lower, ws_rel) && lstat((lower / ws_rel).c_str(), &st) == 0) {
                *path = lower / ws_rel;
                *in_upper = false;
                return true;
            }
            return false;
        }
        struct stat st{};
        if (SymlinkAmongParents(copy, ws_rel) || lstat((copy / ws_rel).c_str(), &st) != 0) return false;
        *path = copy / ws_rel;
        *in_upper = true;
        return true;
    };
    // A directory as the item saw it: the lower copy with the upper layer applied.
    auto merged_tree = [&](const std::string& ws_rel) -> store::Digest {
        const fs::path tmp = g.dir / ("merge-" + RandomHex(4));
        merkle_->MaterializeTree(merkle_->Lookup(spec.input_root, ws_rel).digest, tmp, store::Materialize::Copy);
        std::error_code mec;
        const fs::path src = upper / ws_rel;
        for (auto it = fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, mec);
             !mec && it != fs::recursive_directory_iterator(); it.increment(mec)) {
            const fs::path rel = fs::relative(it->path(), src, mec);
            const fs::path dst = tmp / rel;
            struct stat st{};
            if (lstat(it->path().c_str(), &st) != 0) continue;
            if (IsWhiteout(st)) {
                fs::remove_all(dst, mec);
            } else if (S_ISDIR(st.st_mode)) {
                if (IsOpaqueDir(it->path())) fs::remove_all(dst, mec);
                fs::create_directories(dst, mec);
            } else {
                fs::remove_all(dst, mec);
                fs::copy(it->path(), dst, fs::copy_options::copy_symlinks, mec);
            }
        }
        const store::Digest d = merkle_->BuildTree(tmp);
        ForceRemove(tmp);
        return d;
    };
    auto capture_path = [&](const std::string& rel_cwd, OutputEntry& e) -> bool {
        const std::string ws_rel = JoinRel(cwd, rel_cwd);
        fs::path p;
        bool in_upper = false;
        if (!locate(ws_rel, &p, &in_upper)) return false;
        struct stat st{};
        if (lstat(p.c_str(), &st) != 0) return false;
        e.path = fs::path(rel_cwd).lexically_normal().string();
        if (S_ISLNK(st.st_mode)) {
            e.is_symlink = true;
            e.symlink_target = fs::read_symlink(p, ec).string();
        } else if (S_ISDIR(st.st_mode)) {
            e.is_dir = true;
            const bool merged = overlay_ && in_upper &&
                                merkle_->Lookup(spec.input_root, ws_rel).kind == store::TreeEntry::Kind::Directory;
            e.digest = merged ? merged_tree(ws_rel) : merkle_->BuildTree(p);
        } else if (S_ISREG(st.st_mode)) {
            e.digest = store_->PutBlobFile(p);
            e.is_executable = (st.st_mode & S_IXUSR) != 0;
        } else {
            return false;
        }
        return true;
    };
    auto capture_item = [&](const ItemSpec& it) {
        ItemRunResult r;
        r.id = it.id;
        std::set<std::string> seen;
        std::vector<std::string> missing;
        for (const auto& o : it.outputs) {
            OutputEntry e;
            if (!seen.insert(fs::path(o).lexically_normal().string()).second) continue;
            if (capture_path(o, e))
                r.outputs.push_back(std::move(e));
            else
                missing.push_back(o);
        }
        for (const auto& o : it.optional_outputs) {
            OutputEntry e;
            if (seen.insert(fs::path(o).lexically_normal().string()).second && capture_path(o, e))
                r.outputs.push_back(std::move(e));
        }
        if (it.capture_changed) {
            std::vector<std::string> changed;
            if (overlay_) {
                const fs::path base = upper / cwd;
                for (auto i = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
                     !ec && i != fs::recursive_directory_iterator(); i.increment(ec)) {
                    struct stat st{};
                    if (lstat(i->path().c_str(), &st) != 0 || IsWhiteout(st)) continue;
                    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
                        changed.push_back(fs::relative(i->path(), base, ec).string());
                }
            } else {
                for (const auto& [rel, stamp] : Stamps(copy / cwd)) {
                    auto b4 = before.find(rel);
                    if (b4 == before.end() || b4->second.size != stamp.size || b4->second.mtime_ns != stamp.mtime_ns)
                        changed.push_back(rel);
                }
            }
            std::sort(changed.begin(), changed.end());
            for (const auto& rel : changed) {
                OutputEntry e;
                if (seen.insert(rel).second && capture_path(rel, e)) r.outputs.push_back(std::move(e));
            }
        }
        if (!missing.empty()) {
            r.failure = ItemFailure::MissingOutput;
            r.message = "missing output";
            for (size_t i = 0; i < missing.size() && i < 5; ++i) r.message += (i ? ", " : ": ") + missing[i];
        }
        if (!it.depfile.empty()) {
            fs::path dep;
            bool in_upper = false;
            if (locate(JoinRel(cwd, it.depfile), &dep, &in_upper)) {
                std::ifstream in(dep);
                std::stringstream ss;
                ss << in.rdbuf();
                std::set<std::string> fp;
                for (const auto& p : ParseDepfile(ss.str())) {
                    std::string ws;
                    if (p.starts_with("/work/")) {
                        ws = fs::path(p.substr(6)).lexically_normal().string();
                    } else if (p.front() != '/') {
                        ws = JoinRel(cwd, p);
                    } else {
                        continue; // outside the workspace: the environment's identity covers it
                    }
                    if (SafeRelative(ws) && ws != ".") fp.insert(ws);
                }
                r.footprint.assign(fp.begin(), fp.end());
            }
        }
        return r;
    };

    // ---- the loop ----
    for (;;) {
        if (open_fds > 0) {
            if (poll(fds, 2, 100) > 0) {
                for (int i = 0; i < 2; ++i) {
                    if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                    ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                    if (n > 0) {
                        const std::string_view chunk(buf, static_cast<size_t>(n));
                        output_bytes += static_cast<uint64_t>(n);
                        (i == 0 ? out_file : err_file).write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
                        AppendTail(i == 0 ? out_tail : err_tail, chunk);
                        if (spec.stream_logs && cb.on_log) cb.on_log(first.id, i + 1, chunk);
                    } else {
                        close(fds[i].fd);
                        fds[i].fd = -1;
                        --open_fds;
                    }
                }
            }
        } else {
            usleep(50 * 1000);
        }
        const auto now = Clock::now();
        if (!exited) {
            siginfo_t info{};
            if (waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid) {
                exited = true;
                exited_at = now;
            }
        }
        if (exited && (open_fds == 0 || now - exited_at > std::chrono::seconds(2)))
            break; // a grandchild may hold the pipes
        if (now - last_tick < kTick) continue;

        // Frozen time is not running time: it counts neither against the timeout nor as a hang.
        if (!cg.empty() && cgroup::Frozen(cg)) {
            deadline += now - last_tick;
            last_progress += now - last_tick;
        }
        last_tick = now;
        scan_checkpoint();
        if (watch) {
            for (auto& p : watch->Poll(std::chrono::milliseconds(0)))
                finished_paths.insert(fs::path(p).lexically_normal().string());
            commit_ready(capture_item);
        }
        if (spec.hung_after.count() > 0 && !terminating) {
            const int64_t cpu = cpu_ms_now();
            if (cpu != last_cpu || output_bytes != last_output_bytes) {
                last_cpu = cpu;
                last_output_bytes = output_bytes;
                last_progress = now;
            } else if (now - last_progress >= spec.hung_after) {
                hung = true;
                begin_termination();
            }
        }
        if (!terminating && now >= deadline) {
            timed_out = true;
            begin_termination();
        }
        if (!terminating && stop.stop_requested()) {
            cancelled = true;
            begin_termination();
        }
        if (terminating && !killed && now - term_started >= spec.term_grace) hard_kill();
    }
    for (int i = 0; i < 2; ++i) {
        if (fds[i].fd >= 0) close(fds[i].fd);
    }
    int status = 0;
    struct rusage ru{};
    wait4(pid, &status, 0, &ru);
    const auto end = Clock::now();
    out_file.close();
    err_file.close();
    scan_checkpoint();
    if (watch) {
        for (auto& p : watch->Poll(std::chrono::milliseconds(0)))
            finished_paths.insert(fs::path(p).lexically_normal().string());
    }
    if (cancelled && !timed_out && !hung) return; // unfinished items go back to the requester

    int exit_code = -1;
    if (WIFEXITED(status))
        exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        exit_code = 128 + WTERMSIG(status);
    const bool not_found = exit_code == 1 && err_tail.find("bwrap: execvp ") != std::string::npos;
    if (not_found) exit_code = 127;
    const bool sandbox_failed = !not_found && exit_code == 1 && err_tail.rfind("bwrap: ", 0) == 0;
    const bool oom = !cg.empty() && cgroup::OomKills(cg) > oom_before;

    const store::Digest out_digest = store_->PutBlobFile(g.dir / "stdout");
    const store::Digest err_digest = store_->PutBlobFile(g.dir / "stderr");
    const int64_t wall = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    const int64_t cpu =
        (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000 + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000;

    if (g.combined && exit_code == 0 && !timed_out && !hung && !oom) commit_ready(capture_item);
    for (const ItemSpec* it : g.items) {
        if (committed.contains(it->id)) continue;
        ItemRunResult r;
        if (timed_out) {
            r.id = it->id;
            r.failure = ItemFailure::Timeout;
            r.message = "timed out after " + std::to_string(spec.timeout.count()) + "s";
        } else if (hung) {
            r.id = it->id;
            r.failure = ItemFailure::Hung;
            r.message = "no CPU progress and no output for " + std::to_string(spec.hung_after.count()) + "s";
        } else if (oom) {
            r.id = it->id;
            r.failure = ItemFailure::Oom;
            r.message = "killed: out of memory (limit " + std::to_string(spec.mem_bytes / (1024 * 1024)) + " MiB)";
        } else if (sandbox_failed) {
            r.id = it->id;
            r.failure = ItemFailure::Sandbox;
            r.message = err_tail.substr(0, err_tail.find('\n'));
        } else if (exit_code != 0) {
            r.id = it->id;
            r.failure = ItemFailure::Exit;
            r.message = not_found ? "command not found" : "exit " + std::to_string(exit_code);
        } else {
            r = capture_item(*it);
        }
        r.exit_code = exit_code;
        r.stdout_digest = out_digest;
        r.stderr_digest = err_digest;
        r.stdout_tail = out_tail;
        r.stderr_tail = err_tail;
        r.wall_ms = wall;
        r.cpu_ms = g.combined ? std::max<int64_t>(0, cpu - cpu_at_last_commit) : cpu;
        r.peak_rss = static_cast<int64_t>(ru.ru_maxrss) * 1024;
        if (r.failure != ItemFailure::None && r.message.empty()) r.message = FailureWord(r.failure);
        if (cb.on_item) cb.on_item(std::move(r));
    }
}

} // namespace zeromesh::exec
