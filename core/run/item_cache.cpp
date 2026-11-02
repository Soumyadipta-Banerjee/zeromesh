#include "run/item_cache.hpp"

#include <algorithm>

namespace zeromesh::run {

namespace {

constexpr size_t kFootprintsPerCommand = 8;

bool SamePaths(const v1::Footprint& a, const v1::Footprint& b) {
    if (a.files_size() != b.files_size() || a.listings_size() != b.listings_size()) return false;
    for (int i = 0; i < a.files_size(); ++i) {
        if (a.files(i).path() != b.files(i).path()) return false;
    }
    for (int i = 0; i < a.listings_size(); ++i) {
        if (a.listings(i).path() != b.listings(i).path()) return false;
    }
    return true;
}

} // namespace

ItemCache::ItemCache(std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle)
    : cas_(std::move(cas)), merkle_(std::move(merkle)) {}

std::string ItemCache::FootprintsKey(const std::string& weak) {
    return store::CasStore::ComputeDigest("footprints\n" + weak).hash;
}

bool ItemCache::Complete(const v1::CachedItem& item) const {
    auto has = [&](const v1::Digest& d) { return d.hash().empty() || cas_->HasBlob(d.hash()); };
    if (!has(item.stdout()) || !has(item.stderr())) return false;
    for (const auto& o : item.outputs()) {
        if (o.symlink()) continue;
        if (o.digest().hash().empty()) return false;
        if (!o.is_dir()) {
            if (!cas_->HasBlob(o.digest().hash())) return false;
            continue;
        }
        try {
            for (const auto& d : merkle_->CollectTree(store::Digest{o.digest().hash(), o.digest().size_bytes()})) {
                if (!cas_->HasBlob(d.hash)) return false;
            }
        } catch (const std::exception&) {
            return false;
        }
    }
    return true;
}

std::optional<v1::CachedItem> ItemCache::Get(const std::string& key) const {
    auto raw = cas_->GetActionResult(key);
    if (!raw) return std::nullopt;
    v1::CachedItem item;
    if (!item.ParseFromString(*raw) || !Complete(item)) return std::nullopt;
    return item;
}

void ItemCache::Put(const std::string& key, const v1::CachedItem& item) {
    cas_->PutActionResult(key, item.SerializeAsString(), /*replace=*/true);
}

std::vector<v1::Footprint> ItemCache::Footprints(const std::string& weak) const {
    auto raw = cas_->GetActionResult(FootprintsKey(weak));
    if (!raw) return {};
    v1::WeakFootprints list;
    if (!list.ParseFromString(*raw) || list.weak() != weak) return {};
    return {list.footprints().begin(), list.footprints().end()};
}

void ItemCache::AddFootprint(const std::string& weak, const v1::Footprint& fp) {
    std::lock_guard lock(fp_mu_);
    auto old = Footprints(weak);
    v1::WeakFootprints list;
    list.set_weak(weak);
    *list.add_footprints() = fp;
    for (const auto& f : old) {
        if (static_cast<size_t>(list.footprints_size()) >= kFootprintsPerCommand) break;
        if (!SamePaths(f, fp)) *list.add_footprints() = f;
    }
    cas_->PutActionResult(FootprintsKey(weak), list.SerializeAsString(), /*replace=*/true);
}

v1::CacheLookupReply ItemCache::Lookup(const v1::CacheLookup& req) const {
    v1::CacheLookupReply reply;
    for (const auto& key : req.keys()) {
        if (!store::CasStore::IsValidHash(key)) continue;
        if (auto item = Get(key)) {
            auto* hit = reply.add_hits();
            hit->set_key(key);
            *hit->mutable_item() = std::move(*item);
        }
    }
    for (const auto& weak : req.weaks()) {
        if (!store::CasStore::IsValidHash(weak)) continue;
        auto fps = Footprints(weak);
        if (fps.empty()) continue;
        auto* w = reply.add_footprints();
        w->set_weak(weak);
        for (auto& f : fps) *w->add_footprints() = std::move(f);
    }
    return reply;
}

bool ItemCache::Forget(const std::string& key) {
    return cas_->RemoveActionResult(key);
}

size_t ItemCache::ForgetAll() {
    size_t n = 0;
    for (const auto& key : cas_->ListActionResults()) n += cas_->RemoveActionResult(key) ? 1 : 0;
    return n;
}

void ItemCache::Prune() {
    for (const auto& key : cas_->ListActionResults()) {
        auto raw = cas_->GetActionResult(key);
        if (!raw) continue;
        v1::WeakFootprints list;
        if (list.ParseFromString(*raw) && !list.weak().empty() && FootprintsKey(list.weak()) == key) continue;
        v1::CachedItem item;
        if (!item.ParseFromString(*raw) || !Complete(item)) cas_->RemoveActionResult(key);
    }
}

} // namespace zeromesh::run
