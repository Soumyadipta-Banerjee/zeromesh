#pragma once

#include "cas/merkle.hpp"
#include "cas/store.hpp"
#include "executor/bwrap.hpp"
#include "executor/host_guard.hpp"
#include "executor/landlock.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zeromesh::executor {

struct ExecutionRequest {
    std::string action_id;
    std::vector<std::string> command_args;
    std::vector<std::pair<std::string, std::string>> env_vars;
    cas::Digest input_root_digest;
    std::vector<std::string> expected_output_files;
    std::vector<std::string> expected_output_directories;
    std::string working_directory{"."};
    std::chrono::seconds timeout{120};
};

struct ExecutionResult {
    int exit_code{0};
    std::string stdout_raw;
    cas::Digest stdout_digest;
    std::string stderr_raw;
    cas::Digest stderr_digest;
    std::vector<std::pair<std::string, cas::Digest>> output_files;
    std::chrono::milliseconds execution_duration{0};
    std::string worker_id;
};

/**
 * @brief Coordinates sandboxed hermetic task execution.
 * Materializes input tree, executes inside bwrap + landlock with SCHED_IDLE priority,
 * and harvests output files back into CAS.
 */
class TaskRunner {
public:
    TaskRunner(std::shared_ptr<cas::CasStore> store, std::shared_ptr<cas::MerkleBuilder> merkle);
    ~TaskRunner() = default;

    /**
     * @brief Executes an action request inside an unprivileged hermetic sandbox.
     */
    [[nodiscard]] ExecutionResult Run(const ExecutionRequest& request);

private:
    std::shared_ptr<cas::CasStore> store_;
    std::shared_ptr<cas::MerkleBuilder> merkle_;
    std::filesystem::path sandbox_base_dir_;
};

} // namespace zeromesh::executor
