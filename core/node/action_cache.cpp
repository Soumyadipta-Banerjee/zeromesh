#include "node/action_cache.hpp"

#include "exec/units.hpp"

#include <filesystem>

namespace zeromesh::node {

namespace {

// Marks a TaskResult as a partial-units entry rather than a task result.
constexpr const char* kPartialMarker = "zm:partial-units";

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
    std::lock_guard lock(units_mu_);
    cas_->RemoveActionResult(UnitsKey(task_key)); // the whole task is cached now
}

std::string ActionCache::UnitsKey(const std::string& task_key) {
    return store::CasStore::ComputeDigest(task_key + "/units").hash;
}

std::optional<v1::TaskResult> ActionCache::ReadUnitsEntry(const std::string& units_key) const {
    auto raw = cas_->GetActionResult(units_key);
    if (!raw) return std::nullopt;
    v1::TaskResult r;
    if (!r.ParseFromString(*raw) || r.error() != kPartialMarker) return std::nullopt;
    return r;
}

void ActionCache::WriteUnitsEntry(const std::string& units_key, const v1::TaskResult& entry) {
    // PutActionResult keeps an existing entry, so replace by removing first. A reader that
    // lands between the two sees no units, which only costs a unit being recomputed.
    cas_->RemoveActionResult(units_key);
    if (entry.outputs_size() > 0) cas_->PutActionResult(units_key, entry.SerializeAsString());
}

void ActionCache::PutUnit(const std::string& task_key, const std::string& unit_id, const v1::Digest& digest) {
    if (!exec::IsValidUnitId(unit_id) || !store::CasStore::IsValidHash(digest.hash())) return;
    const std::string units_key = UnitsKey(task_key);
    std::lock_guard lock(units_mu_);
    v1::TaskResult entry;
    if (auto existing = ReadUnitsEntry(units_key)) {
        entry = std::move(*existing);
    } else {
        entry.set_exit_code(-1);
        entry.set_error(kPartialMarker);
    }
    bool replaced = false;
    for (auto& o : *entry.mutable_outputs()) {
        if (o.path() == unit_id) {
            *o.mutable_digest() = digest;
            replaced = true;
        }
    }
    if (!replaced) {
        auto* o = entry.add_outputs();
        o->set_path(unit_id);
        *o->mutable_digest() = digest;
    }
    WriteUnitsEntry(units_key, entry);
}

std::map<std::string, v1::Digest> ActionCache::GetUnits(const std::string& task_key) const {
    std::map<std::string, v1::Digest> out;
    std::lock_guard lock(units_mu_);
    auto entry = ReadUnitsEntry(UnitsKey(task_key));
    if (!entry) return out;
    for (const auto& o : entry->outputs()) {
        if (!exec::IsValidUnitId(o.path()) || !store::CasStore::IsValidHash(o.digest().hash())) continue;
        if (!cas_->HasBlob(o.digest().hash())) continue;
        out[o.path()] = o.digest();
    }
    return out;
}

void ActionCache::PruneStale() {
    const auto ac_dir = cas_->GetBasePath() / "ac";
    std::error_code ec;
    if (!std::filesystem::exists(ac_dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(ac_dir, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::string key = entry.path().filename().string();
        {
            std::lock_guard lock(units_mu_);
            if (auto partial = ReadUnitsEntry(key)) {
                v1::TaskResult kept;
                kept.set_exit_code(-1);
                kept.set_error(kPartialMarker);
                for (const auto& o : partial->outputs()) {
                    if (cas_->HasBlob(o.digest().hash())) *kept.add_outputs() = o;
                }
                if (kept.outputs_size() != partial->outputs_size()) WriteUnitsEntry(key, kept);
                continue;
            }
        }
        if (!Get(key).has_value()) cas_->RemoveActionResult(key);
    }
}

} // namespace zeromesh::node
