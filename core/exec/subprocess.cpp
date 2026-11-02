#include "exec/subprocess.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>

extern char** environ;

namespace zeromesh::exec {

std::string FindOnPath(const std::string& name) {
    if (name.empty()) return {};
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : std::string();
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        const std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate.string();
    }
    return {};
}

CommandResult RunCommand(const std::vector<std::string>& argv, const CommandOptions& opts) {
    CommandResult r;
    if (argv.empty()) {
        r.error = "empty command";
        return r;
    }
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) {
        r.error = std::strerror(errno);
        return r;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, out[1], STDERR_FILENO);
    if (!opts.cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, opts.cwd.c_str());

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setpgroup(&attr, 0);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<std::string> copy = argv;
    std::vector<char*> cargv;
    for (auto& a : copy) cargv.push_back(a.data());
    cargv.push_back(nullptr);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(out[1]);
    if (rc != 0) {
        close(out[0]);
        r.error = std::strerror(rc);
        return r;
    }

    const auto deadline = std::chrono::steady_clock::now() + opts.timeout;
    char buf[4096];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            kill(-pid, SIGKILL);
            r.timed_out = true;
            break;
        }
        pollfd p{out[0], POLLIN, 0};
        const int n = poll(&p, 1, static_cast<int>(std::min<int64_t>(left.count(), 1000)));
        if (n < 0 && errno != EINTR) break;
        if (n <= 0) continue;
        const ssize_t got = read(out[0], buf, sizeof(buf));
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got <= 0) break; // EOF: every writer is gone
        const size_t room = opts.max_output > r.output.size() ? opts.max_output - r.output.size() : 0;
        r.output.append(buf, std::min(room, static_cast<size_t>(got)));
    }
    close(out[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    return r;
}

} // namespace zeromesh::exec
