#include "reapi/action_cache.hpp"

namespace zeromesh::reapi {

ActionCacheService::ActionCacheService(std::shared_ptr<cas::CasStore> store)
    : store_(std::move(store)) {}

std::optional<std::string> ActionCacheService::GetActionResult(const cas::Digest& action_digest) {
    if (!store_) return std::nullopt;
    return store_->GetActionResult(action_digest.hash);
}

bool ActionCacheService::UpdateActionResult(const cas::Digest& action_digest, std::string_view serialized_result) {
    if (!store_) return false;
    return store_->PutActionResult(action_digest.hash, serialized_result);
}

} // namespace zeromesh::reapi
