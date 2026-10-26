#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace zeromesh::mirror {

/**
 * @brief LRU disk-backed chunk cache with configurable size limit.
 *
 * Revived from frozen/vfs/chunk_fetcher without DHT dependencies.
 * Chunks are stored as files under cache_dir, keyed by SHA-256 hash.
 * An in-memory LRU list tracks access order for eviction.
 */
class MirrorCache {
public:
    MirrorCache(std::filesystem::path cache_dir, size_t max_bytes);
    ~MirrorCache() = default;

    MirrorCache(const MirrorCache&) = delete;
    MirrorCache& operator=(const MirrorCache&) = delete;

    /// Retrieves a cached chunk by hash, or std::nullopt if not cached.
    /// Promotes the chunk to the front of the LRU list on hit.
    [[nodiscard]] std::optional<std::string> Get(const std::string& chunk_hash);

    /// Inserts a chunk into the cache, evicting old entries if needed.
    void Put(const std::string& chunk_hash, const std::string& data);

    /// Checks if a chunk exists in the cache.
    [[nodiscard]] bool Contains(const std::string& chunk_hash) const;

    /// Returns the current total cached bytes.
    [[nodiscard]] size_t CurrentBytes() const;

    /// Returns the number of evictions since creation.
    [[nodiscard]] uint64_t EvictionCount() const;

    /// Returns the cache directory.
    [[nodiscard]] const std::filesystem::path& CacheDir() const { return cache_dir_; }

private:
    struct CacheEntry {
        std::string hash;
        size_t size_bytes{0};
    };

    void Evict();

    std::filesystem::path cache_dir_;
    size_t max_bytes_{512 * 1024 * 1024};
    size_t current_bytes_{0};
    uint64_t eviction_count_{0};

    mutable std::mutex mutex_;
    std::list<CacheEntry> lru_list_;
    std::unordered_map<std::string, std::list<CacheEntry>::iterator> lru_map_;
};

} // namespace zeromesh::mirror
