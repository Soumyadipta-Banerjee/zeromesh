#include "node/action_cache.hpp"

namespace zeromesh::node {

namespace {

// An empty hash means "no content" (e.g. a result that never produced a stream).
bool Present(const store::CasStore& cas, const v1::Digest& d) {
    return d.hash().empty() || cas.HasBlob(d.hash());
}

} // namespace

ActionCache::ActionCache(std::shared_ptr<store::CasStore> cas) : cas_(std::move(cas)) {}

std::optional<v1::TaskResult> ActionCache::Get(const std::string& task_key) const {
    auto raw = cas_->GetActionResult(task_key);
    if (!raw) return std::nullopt;
    v1::TaskResult r;
    if (!r.ParseFromString(*raw)) return std::nullopt;
    if (r.exit_code() != 0 || r.timed_out() || r.cancelled() || !r.error().empty()) return std::nullopt;
    if (!Present(*cas_, r.stdout()) || !Present(*cas_, r.stderr())) return std::nullopt;
    for (const auto& o : r.outputs()) {
        if (o.is_symlink()) continue;
        if (o.digest().hash().empty() || !cas_->HasBlob(o.digest().hash())) return std::nullopt;
    }
    return r;
}

void ActionCache::Put(const std::string& task_key, const v1::TaskResult& result) {
    v1::TaskResult r = result;
    r.set_task_key(task_key);
    r.set_cache_hit(false);
    cas_->PutActionResult(task_key, r.SerializeAsString());
}

} // namespace zeromesh::node
