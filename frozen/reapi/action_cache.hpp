#pragma once

#include "cas/store.hpp"

#include <memory>
#include <optional>
#include <string>

namespace zeromesh::reapi {

/**
 * @brief REAPI v2 ActionCache service interface.
 * Answers cache hits to skip re-compilation of identical action digests.
 */
class ActionCacheService {
public:
    explicit ActionCacheService(std::shared_ptr<cas::CasStore> store);
    virtual ~ActionCacheService() = default;

    /**
     * @brief Fetches previously executed ActionResult from cache.
     */
    [[nodiscard]] std::optional<std::string> GetActionResult(const cas::Digest& action_digest);

    /**
     * @brief Caches completed ActionResult for an action digest.
     */
    bool UpdateActionResult(const cas::Digest& action_digest, std::string_view serialized_result);

private:
    std::shared_ptr<cas::CasStore> store_;
};

} // namespace zeromesh::reapi
