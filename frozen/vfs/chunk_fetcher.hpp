#pragma once

#include "cas/store.hpp"
#include "dht/kademlia.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeromesh::vfs {

// ─────────────────────────────────────────────────────────────────────────────
// ChunkFetcher — Bridges DHT provider discovery with TCP chunk retrieval
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Fetch result returned by the ChunkFetcher.
 */
struct ChunkResult {
    bool success{false};
    std::string data;              ///< Raw chunk bytes on success.
    std::string source_peer_ip;    ///< IP of the peer that served the chunk.
    std::chrono::microseconds rtt{0};
};

/**
 * @brief Configuration for the chunk fetcher subsystem.
 */
struct ChunkFetcherConfig {
    /// Timeout for a single TCP chunk download attempt.
    std::chrono::milliseconds fetch_timeout{5000};

    /// Maximum concurrent in-flight chunk fetches.
    size_t max_concurrent_fetches{8};

    /// TCP port to connect to on remote peers for chunk serving.
    uint16_t remote_chunk_port{8984};

    /// TCP port we listen on to serve our local chunks.
    uint16_t local_serve_port{8984};

    /// Maximum retry attempts per chunk before declaring failure.
    size_t max_retries{3};
};

/**
 * @brief LRU disk-backed chunk cache with configurable size limit.
 *
 * Chunks are stored as files under the cache directory, keyed by SHA-256 hash.
 * An in-memory LRU list tracks access order for eviction.
 */
class ChunkCache {
public:
    /**
     * @param cache_dir   Directory for cached chunk files.
     * @param max_bytes   Maximum total cache size; oldest chunks evicted first.
     */
    ChunkCache(std::filesystem::path cache_dir, size_t max_bytes);
    ~ChunkCache() = default;

    /**
     * @brief Retrieves a cached chunk by hash, or std::nullopt if not cached.
     * Promotes the chunk to the front of the LRU list on hit.
     */
    [[nodiscard]] std::optional<std::string> Get(const std::string& chunk_hash);

    /**
     * @brief Inserts a chunk into the cache, evicting old entries if needed.
     */
    void Put(const std::string& chunk_hash, const std::string& data);

    /**
     * @brief Checks if a chunk exists in the cache.
     */
    [[nodiscard]] bool Contains(const std::string& chunk_hash) const;

    /**
     * @brief Returns the current total cached bytes.
     */
    [[nodiscard]] size_t CurrentBytes() const;

    /**
     * @brief Returns the number of evictions since creation.
     */
    [[nodiscard]] uint64_t EvictionCount() const;

private:
    void Evict();

    std::filesystem::path cache_dir_;
    size_t max_bytes_;
    mutable std::mutex mutex_;

    /// LRU list: front = most recently used.
    struct CacheEntry {
        std::string hash;
        size_t size_bytes{0};
    };
    std::list<CacheEntry> lru_list_;
    std::unordered_map<std::string, std::list<CacheEntry>::iterator> lru_map_;
    size_t current_bytes_{0};
    uint64_t eviction_count_{0};
};

/**
 * @brief Fetches file chunks from the DHT provider network.
 *
 * On a cache miss, the fetcher:
 * 1. Queries the Kademlia DHT for providers of the chunk hash.
 * 2. Ranks providers by RTT / locality.
 * 3. Opens a TCP connection to the best provider and streams the chunk.
 * 4. Verifies the received data against the expected SHA-256 hash.
 * 5. Stores the verified chunk in the local CAS store and chunk cache.
 *
 * Also runs a background TCP server to serve local chunks to peers.
 */
class ChunkFetcher {
public:
    ChunkFetcher(ChunkFetcherConfig config,
                 std::shared_ptr<dht::KademliaDht> dht,
                 std::shared_ptr<cas::CasStore> cas_store,
                 std::shared_ptr<ChunkCache> cache);
    ~ChunkFetcher();

    // Non-copyable
    ChunkFetcher(const ChunkFetcher&) = delete;
    ChunkFetcher& operator=(const ChunkFetcher&) = delete;

    /**
     * @brief Starts the chunk server and fetcher threads.
     */
    bool Start();

    /**
     * @brief Stops the chunk server and waits for in-flight fetches.
     */
    void Stop();

    /**
     * @brief Fetches a chunk by hash, checking cache → CAS → DHT in order.
     *
     * Blocks until the chunk is available or all retries are exhausted.
     */
    ChunkResult FetchChunk(const std::string& chunk_hash);

    /**
     * @brief Asynchronous chunk fetch with callback.
     */
    void FetchChunkAsync(const std::string& chunk_hash,
                         std::function<void(ChunkResult)> callback);

    /**
     * @brief Prefetches a batch of chunks in the background.
     * Used for sequential read-ahead optimization.
     */
    void PrefetchChunks(const std::vector<std::string>& chunk_hashes);

    /**
     * @brief Announces a locally available chunk to the DHT network.
     */
    void AnnounceLocalChunk(const std::string& chunk_hash);

    /**
     * @brief Directly fetches a chunk from a specific peer IP and port, bypassing DHT provider resolution.
     * Caches and stores verified chunk in CAS.
     */
    ChunkResult FetchFromPeerDirect(const std::string& ip, uint16_t port, const std::string& chunk_hash);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeromesh::vfs
