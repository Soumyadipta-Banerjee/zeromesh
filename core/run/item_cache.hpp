#pragma once

// The item cache of one node. Results are stored under an item's full key (see run/item_key); the
// footprints recorded for a command are stored under its command ("weak") key, newest first, so a
// later run can compute the full key from the files the command actually read last time. Peers
// query each other's caches through NodeApi.LookupCache, which makes the cache team-wide.

#include "store/merkle.hpp"
#include "store/store.hpp"
#include "zeromesh/v1/node.pb.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::run {

class ItemCache {
public:
    ItemCache(std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle);

    // The result under `key`, only if every blob it names is still in this node's store.
    [[nodiscard]] std::optional<v1::CachedItem> Get(const std::string& key) const;
    void Put(const std::string& key, const v1::CachedItem& item);

    // Footprints recorded for the command `weak`, newest first.
    [[nodiscard]] std::vector<v1::Footprint> Footprints(const std::string& weak) const;
    // Records a footprint (replacing an older one over the same paths); keeps the newest few.
    void AddFootprint(const std::string& weak, const v1::Footprint& fp);

    // Answers a peer's lookup from this node's entries.
    [[nodiscard]] v1::CacheLookupReply Lookup(const v1::CacheLookup& req) const;

    bool Forget(const std::string& key);
    size_t ForgetAll();

    // Drops results whose blobs are gone (after CAS garbage collection).
    void Prune();

private:
    static std::string FootprintsKey(const std::string& weak);
    [[nodiscard]] bool Complete(const v1::CachedItem& item) const;

    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    mutable std::mutex fp_mu_; // read-modify-write of footprint lists
};

} // namespace zeromesh::run
