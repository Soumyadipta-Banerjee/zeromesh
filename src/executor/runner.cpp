#include "executor/runner.hpp"

#include <chrono>
#include <iostream>

namespace zeromesh::executor {

TaskRunner::TaskRunner(std::shared_ptr<cas::CasStore> store, std::shared_ptr<cas::MerkleBuilder> merkle)
    : store_(std::move(store)), merkle_(std::move(merkle)) {
    sandbox_base_dir_ = std::filesystem::temp_directory_path() / "zeromesh" / "sandboxes";
    std::filesystem::create_directories(sandbox_base_dir_);
}

ExecutionResult TaskRunner::Run(const ExecutionRequest& request) {
    auto start_time = std::chrono::steady_clock::now();

    // Prepare execution result structure
    ExecutionResult result{
        .exit_code = 0,
        .stdout_raw = "",
        .stdout_digest = cas::Digest{"", 0},
        .stderr_raw = "",
        .stderr_digest = cas::Digest{"", 0},
        .output_files = {},
        .execution_duration = std::chrono::milliseconds(0),
        .worker_id = "local-runner"
    };

    if (request.command_args.empty()) {
        result.exit_code = 1;
        result.stderr_raw = "Error: empty command arguments";
        return result;
    }

    auto end_time = std::chrono::steady_clock::now();
    result.execution_duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    return result;
}

} // namespace zeromesh::executor
