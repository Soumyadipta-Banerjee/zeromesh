#pragma once

#include "dht/kademlia.hpp"
#include "dht/types.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeromesh::dht {

// ─────────────────────────────────────────────────────────────────────────────
// Local Value Store: TTL-managed key-value entries
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A single stored key-value entry with expiration metadata.
 */
struct ValueEntry {
    Key key{};
    std::string value;
    NodeId publisher_id{};               ///< Node that originally published this entry.
    std::chrono::steady_clock::time_point stored_at;
    std::chrono::steady_clock::time_point expires_at;
};

/**
 * @brief Thread-safe local storage backend for DHT key-value pairs and
 *        provider records.
 *
 * Manages two distinct registries:
 * 1. **Value Store** — small key-value blobs (Action Cache results, DSM leases).
 * 2. **Provider Store** — lightweight provider records pointing to nodes that
 *    hold specific CAS chunks (never stores chunk data itself).
 *
 * Both registries enforce TTL-based automatic expiry. The store is purely local;
 * replication to remote nodes is handled by the KademliaDht engine.
 */
class LocalStore {
public:
    LocalStore() = default;
    ~LocalStore() = default;

    // Non-copyable
    LocalStore(const LocalStore&) = delete;
    LocalStore& operator=(const LocalStore&) = delete;

    // ── Value Operations ────────────────────────────────────────────────

    /**
     * @brief Stores a key-value entry with the given TTL.
     *
     * Overwrites any existing entry for the same key. Rejects values
     * exceeding kMaxValueSize.
     *
     * @return true if stored successfully, false if value too large.
     */
    bool PutValue(const Key& key, const std::string& value,
                  const NodeId& publisher_id,
                  std::chrono::seconds ttl = std::chrono::seconds(3600));

    /**
     * @brief Retrieves a stored value by key, or std::nullopt if expired or absent.
     */
    [[nodiscard]] std::optional<std::string> GetValue(const Key& key) const;

    /**
     * @brief Checks if a non-expired value exists for the given key.
     */
    [[nodiscard]] bool HasValue(const Key& key) const;

    /**
     * @brief Removes a specific value entry.
     */
    bool RemoveValue(const Key& key);

    // ── Provider Operations ─────────────────────────────────────────────

    /**
     * @brief Registers a node as a provider for a given chunk hash.
     *
     * If the provider already exists for this chunk, its timestamp is refreshed.
     * Provider records expire after kProviderTtl unless re-announced.
     */
    void AddProvider(const Key& chunk_hash, const PeerContact& provider);

    /**
     * @brief Returns all non-expired providers for a given chunk hash.
     */
    [[nodiscard]] std::vector<PeerContact> GetProviders(const Key& chunk_hash) const;

    /**
     * @brief Removes a specific provider record.
     */
    bool RemoveProvider(const Key& chunk_hash, const NodeId& provider_id);

    // ── Maintenance ─────────────────────────────────────────────────────

    /**
     * @brief Evicts all expired value and provider entries.
     *
     * Should be called periodically (e.g., every kRefreshInterval) by the
     * DHT engine's maintenance loop.
     *
     * @return Total number of entries evicted.
     */
    size_t PurgeExpired();

    /**
     * @brief Returns total count of stored values (including expired).
     */
    [[nodiscard]] size_t ValueCount() const;

    /**
     * @brief Returns total count of unique chunk hashes with active providers.
     */
    [[nodiscard]] size_t ProviderKeyCount() const;

    /**
     * @brief Returns all locally stored value entries that should be
     *        republished (approaching their TTL midpoint).
     */
    [[nodiscard]] std::vector<ValueEntry> EntriesNeedingRepublish() const;

private:
    // ── Value storage ───────────────────────────────────────────────────

    /// Hash functor for Key (std::array<uint8_t,32>) used in unordered_map.
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            // FNV-1a over the first 8 bytes (sufficient for hash table distribution)
            size_t h = 14695981039346656037ULL;
            for (size_t i = 0; i < 8 && i < k.size(); ++i) {
                h ^= static_cast<size_t>(k[i]);
                h *= 1099511628211ULL;
            }
            return h;
        }
    };

    mutable std::mutex value_mutex_;
    std::unordered_map<Key, ValueEntry, KeyHash> values_;

    // ── Provider storage ────────────────────────────────────────────────

    struct ProviderEntry {
        PeerContact contact;
        std::chrono::steady_clock::time_point added_at;
        std::chrono::steady_clock::time_point expires_at;
    };

    mutable std::mutex provider_mutex_;
    std::unordered_map<Key, std::vector<ProviderEntry>, KeyHash> providers_;
};

} // namespace zeromesh::dht
