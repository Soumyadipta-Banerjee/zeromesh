#pragma once

// Finding and running helper programs (bwrap --version, strace, the dynamic loader) from the
// daemon: no shell, stdin from /dev/null, output captured, a deadline, and the whole process
// group killed when it passes.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace zeromesh::exec {

// `name` when it contains a '/' and is executable; otherwise the first executable PATH entry
// (PATH unset: /usr/bin:/bin); "" when there is none.
std::string FindOnPath(const std::string& name);

struct CommandOptions {
    std::filesystem::path cwd;                            // empty: the caller's
    std::chrono::milliseconds timeout{std::chrono::seconds(60)};
    size_t max_output{1 << 20};                           // bytes kept; the rest is read and dropped
};

struct CommandResult {
    std::string error;     // the command could not be started (e.g. "No such file or directory")
    bool timed_out{false}; // killed at the deadline
    int exit_code{-1};     // -1 when killed by a signal
    std::string output;    // stdout and stderr as written, at most max_output bytes

    [[nodiscard]] bool ok() const { return error.empty() && !timed_out && exit_code == 0; }
};

// Runs argv (argv[0] searched on PATH) in its own process group with the default signal mask
// and dispositions, so a daemon that blocks signals for signalfd does not pass that on.
CommandResult RunCommand(const std::vector<std::string>& argv, const CommandOptions& opts = {});

} // namespace zeromesh::exec
