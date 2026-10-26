#pragma once

#include "cas/store.hpp"
#include "executor/runner.hpp"
#include "reapi/action_cache.hpp"
#include "scheduler/scheduler.hpp"

#include <memory>
#include <optional>
#include <string>

namespace zeromesh::reapi {

/**
 * @brief REAPI v2 Execution service interface.
 * Coordinates action execution with an explicit Cache-First Short-Circuit:
 * checks ActionCache before contacting Scheduler or dispatching to any peer cores.
 */
class ExecutionService {
public:
    ExecutionService(
        std::shared_ptr<cas::CasStore> store,
        std::shared_ptr<ActionCacheService> action_cache,
        std::shared_ptr<scheduler::Scheduler> scheduler,
        std::shared_ptr<executor::TaskRunner> runner
    );
    virtual ~ExecutionService() = default;

    /**
     * @brief Executes an action request.
     * Enforces Cache-First Short-Circuit:
     *  1. Checks ActionCache for action_digest.
     *  2. If hit, immediately returns cached result without invoking scheduler or peer cores.
     *  3. If miss, queries Scheduler for local vs. remote peer dispatch.
     *  4. Stores completed ActionResult in ActionCache.
     */
    executor::ExecutionResult ExecuteAction(const executor::ExecutionRequest& request, bool skip_cache = false);

    /**
     * @brief Fast-path ActionCache lookup.
     */
    [[nodiscard]] std::optional<std::string> CheckActionCache(const cas::Digest& action_digest) const;

private:
    std::shared_ptr<cas::CasStore> store_;
    std::shared_ptr<ActionCacheService> action_cache_;
    std::shared_ptr<scheduler::Scheduler> scheduler_;
    std::shared_ptr<executor::TaskRunner> runner_;
};

} // namespace zeromesh::reapi
