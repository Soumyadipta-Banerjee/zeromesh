#include "executor/runner.hpp"
#include "executor/bwrap.hpp"
#include "executor/host_guard.hpp"
#include "executor/landlock.hpp"

#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace zeromesh::executor {

TaskRunner::TaskRunner(std::shared_ptr<cas::CasStore> store, std::shared_ptr<cas::MerkleBuilder> merkle)
    : store_(std::move(store)), merkle_(std::move(merkle)) {
    sandbox_base_dir_ = std::filesystem::temp_directory_path() / "zeromesh" / "sandboxes";
    std::filesystem::create_directories(sandbox_base_dir_);
}

ExecutionResult TaskRunner::Run(const ExecutionRequest& request) {
    auto start_time = std::chrono::steady_clock::now();

    ExecutionResult result{
        .exit_code = 0,
        .stdout_raw = "",
        .stdout_digest = cas::Digest{"", 0},
        .stderr_raw = "",
        .stderr_digest = cas::Digest{"", 0},
        .output_files = {},
        .execution_duration = std::chrono::milliseconds(0),
        .worker_id = "local-sandbox"
    };

    if (request.command_args.empty()) {
        result.exit_code = 1;
        result.stderr_raw = "Error: empty command arguments";
        return result;
    }

    // 1. Prepare isolated sandbox workspace
    auto sandbox_dir = sandbox_base_dir_ / ("action_" + request.action_id);
    std::filesystem::remove_all(sandbox_dir);
    std::filesystem::create_directories(sandbox_dir);

    // 2. Materialize input Merkle tree if digest is supplied
    if (merkle_ && !request.input_root_digest.hash.empty()) {
        merkle_->MaterializeTree(request.input_root_digest, sandbox_dir);
    }

    auto work_path = sandbox_dir / request.working_directory;
    std::filesystem::create_directories(work_path);

    // 3. Create non-blocking communication pipes for stdout and stderr
    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe2(stdout_pipe, O_CLOEXEC) < 0 || pipe2(stderr_pipe, O_CLOEXEC) < 0) {
        result.exit_code = -1;
        result.stderr_raw = "Error: failed to create execution pipes";
        return result;
    }

    // 4. Fork child process
    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        result.exit_code = -1;
        result.stderr_raw = "Error: fork failed";
        return result;
    }

    if (pid == 0) {
        // --- Child Execution Process ---
        // Redirect standard output and error to write ends of pipes
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        // Enforce SCHED_IDLE task priority to ensure host donor responsiveness
        HostGuard::EnforceIdlePriority(0);

        // Apply custom environment variables
        for (const auto& [key, val] : request.env_vars) {
            setenv(key.c_str(), val.c_str(), 1);
        }

        // Change directory into workspace
        chdir(work_path.c_str());

        // Apply Landlock LSM kernel sandbox if supported
        if (LandlockSandbox::IsSupported()) {
            LandlockSandbox ll;
            ll.AllowReadOnly("/usr");
            ll.AllowReadOnly("/lib");
            ll.AllowReadOnly("/lib64");
            ll.AllowReadOnly("/bin");
            ll.AllowReadOnly("/etc");
            ll.AllowReadWrite(sandbox_dir);
            ll.AllowReadWrite("/tmp");
            ll.RestrictCurrentProcess();
        }

        // Assemble argv
        std::vector<char*> argv;
        for (const auto& arg : request.command_args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(argv[0], argv.data());
        std::cerr << "zeromesh: failed to execute " << argv[0] << "\n";
        _exit(127);
    }

    // --- Parent Process ---
    // Close write ends
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    std::string stdout_acc;
    std::string stderr_acc;

    struct pollfd fds[2];
    fds[0].fd = stdout_pipe[0];
    fds[0].events = POLLIN | POLLHUP;
    fds[1].fd = stderr_pipe[0];
    fds[1].events = POLLIN | POLLHUP;

    int open_pipes = 2;
    char buffer[4096];

    while (open_pipes > 0) {
        int ret = poll(fds, 2, 5000); // 5s poll timeout
        if (ret <= 0) {
            // Check if child has exited
            int status = 0;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid || w < 0) {
                break;
            }
            continue;
        }

        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd >= 0 && (fds[i].revents & (POLLIN | POLLHUP))) {
                ssize_t bytes = read(fds[i].fd, buffer, sizeof(buffer));
                if (bytes > 0) {
                    if (i == 0) {
                        stdout_acc.append(buffer, static_cast<size_t>(bytes));
                    } else {
                        stderr_acc.append(buffer, static_cast<size_t>(bytes));
                    }
                } else if (bytes == 0 || (fds[i].revents & POLLHUP)) {
                    close(fds[i].fd);
                    fds[i].fd = -1;
                    open_pipes--;
                }
            }
        }
    }

    if (fds[0].fd >= 0) close(fds[0].fd);
    if (fds[1].fd >= 0) close(fds[1].fd);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }

    // 5. Harvest output artifacts into CAS
    if (store_) {
        for (const auto& out_file : request.expected_output_files) {
            auto out_path = work_path / out_file;
            if (std::filesystem::exists(out_path) && std::filesystem::is_regular_file(out_path)) {
                std::ifstream file(out_path, std::ios::binary);
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                auto d = store_->PutBlob(content);
                result.output_files.emplace_back(out_file, d);
            }
        }

        result.stdout_raw = stdout_acc;
        result.stdout_digest = store_->PutBlob(stdout_acc);
        result.stderr_raw = stderr_acc;
        result.stderr_digest = store_->PutBlob(stderr_acc);
    } else {
        result.stdout_raw = stdout_acc;
        result.stderr_raw = stderr_acc;
    }

    // 6. Clean up temporary sandbox directory
    std::error_code ec;
    std::filesystem::remove_all(sandbox_dir, ec);

    auto end_time = std::chrono::steady_clock::now();
    result.execution_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    return result;
}

} // namespace zeromesh::executor
