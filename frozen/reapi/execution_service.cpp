#include "reapi/execution_service.hpp"

#include <iostream>

namespace zeromesh::reapi {

ExecutionService::ExecutionService(
    std::shared_ptr<cas::CasStore> store,
    std::shared_ptr<ActionCacheService> action_cache,
    std::shared_ptr<scheduler::Scheduler> scheduler,
    std::shared_ptr<executor::TaskRunner> runner
) : store_(std::move(store)),
    action_cache_(std::move(action_cache)),
    scheduler_(std::move(scheduler)),
    runner_(std::move(runner)) {}

std::optional<std::string> ExecutionService::CheckActionCache(const cas::Digest& action_digest) const {
    if (!action_cache_) return std::nullopt;
    return action_cache_->GetActionResult(action_digest);
}

executor::ExecutionResult ExecutionService::ExecuteAction(const executor::ExecutionRequest& request, bool skip_cache) {
    cas::Digest action_digest{request.action_id, 0};

    // Refinement 2: Cache-First Short-Circuit
    // Enforce an explicit ActionCache lookup before hitting the scheduler.
    // If a task digest matches an existing ActionResult in CAS, return immediately.
    if (!skip_cache && action_cache_) {
        auto cached = action_cache_->GetActionResult(action_digest);
        if (cached.has_value()) {
            return executor::ExecutionResult{
                .exit_code = 0,
                .stdout_raw = "",
                .stdout_digest = cas::Digest{"cached", 0},
                .stderr_raw = "",
                .stderr_digest = cas::Digest{"cached", 0},
                .output_files = {},
                .execution_duration = std::chrono::milliseconds(1),
                .worker_id = "cache-hit"
            };
        }
    }

    // Cache miss: proceed to locality-aware scheduler
    scheduler::ActionTask task{
        .action_digest_hash = request.action_id,
        .command_args = request.command_args,
        .env_vars = request.env_vars,
        .input_root_digest_hash = request.input_root_digest.hash,
        .total_input_bytes = request.input_root_digest.size_bytes,
        .missing_bytes_on_peer = 0,
        .working_directory = request.working_directory
    };

    double local_load = 15.0; // host baseline load
    auto decision = scheduler_ ? scheduler_->Decide(task, local_load) : scheduler::ScheduleDecision{};

    executor::ExecutionResult result;
    if (decision.target == scheduler::DispatchTarget::Local && runner_) {
        result = runner_->Run(request);
    } else {
        result = runner_ ? runner_->Run(request) : executor::ExecutionResult{.exit_code = 0, .worker_id = "local"};
    }

    // Cache successful execution result
    if (result.exit_code == 0 && action_cache_) {
        action_cache_->UpdateActionResult(action_digest, "exit_code:0");
    }

    return result;
}

} // namespace zeromesh::reapi
