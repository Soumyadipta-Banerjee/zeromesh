#include "exec/sandbox_runner.hpp"

#include "exec/bwrap.hpp"
#include "exec/priority.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sodium.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <map>
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
        struct stat st {};
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
    struct stat st {};
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
        struct stat st {};
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

} // namespace

SandboxRunner::SandboxRunner(std::shared_ptr<store::CasStore> store, std::shared_ptr<store::MerkleBuilder> merkle,
                             fs::path scratch_base)
    : store_(std::move(store)), merkle_(std::move(merkle)), scratch_base_(std::move(scratch_base)) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium init failed");
    fs::create_directories(scratch_base_);
    bwrap_path_ = FindOnPath("bwrap");
}

RunResult SandboxRunner::Run(const RunSpec& spec, const LogSink& sink, std::stop_token stop) {
    auto start = std::chrono::steady_clock::now();
    RunResult result;

    if (spec.argv.empty()) {
        result.error = "empty command";
        return result;
    }
    if (bwrap_path_.empty()) {
        result.error = "bubblewrap (bwrap) is not installed on this node";
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

    Snapshot before;
    if (spec.capture_changed) before = TakeSnapshot(work);

    BwrapBuilder b;
    b.SetBinary(bwrap_path_).ShareNetwork(spec.allow_network).AddDev("/dev").AddProc("/proc");
    for (const auto& s : spec.sandbox_env.symlinks) b.AddSymlink(s.target, s.link);
    for (const auto& m : spec.sandbox_env.binds) {
        if (m.read_only) b.AddReadOnlyBind(m.host, m.sandbox);
        else b.AddReadWriteBind(m.host, m.sandbox);
    }
    b.AddTmpfs("/tmp").AddReadWriteBind(work.string(), "/work");
    b.AddEnv("HOME", "/tmp").AddEnv("ZM_NODE_ID", spec.node_id);
    for (const auto& [k, v] : spec.sandbox_env.env) b.AddEnv(k, v);
    for (const auto& [k, v] : spec.env) b.AddEnv(k, v);
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

    pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        ForceRemove(scratch);
        return result;
    }
    if (pid == 0) {
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
            if (stop.stop_requested()) result.cancelled = true;
            else result.timed_out = true;
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
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    // bwrap reports a missing command as exit 1 + "bwrap: execvp ..."; use the shell convention.
    if (result.exit_code == 1 && result.stderr_head.rfind("bwrap: execvp ", 0) == 0) result.exit_code = 127;

    out_file.close();
    err_file.close();
    result.stdout_digest = store_->PutBlobFile(scratch / "stdout");
    result.stderr_digest = store_->PutBlobFile(scratch / "stderr");

    auto capture = [&](const std::string& rel) {
        if (!SafeOutputPath(work, rel)) return;
        fs::path p = work / fs::path(rel).lexically_normal();
        struct stat st {};
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
                bool changed = it == before.end() || it->second.size != stamp.size || it->second.mtime_ns != stamp.mtime_ns;
                if (changed && seen.emplace(rel, true).second) capture(rel);
            }
        }
    }

    ForceRemove(scratch);
    result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return result;
}

} // namespace zeromesh::exec
