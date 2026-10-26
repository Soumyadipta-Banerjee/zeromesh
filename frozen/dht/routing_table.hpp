#pragma once

#include "dht/kademlia.hpp"
#include "dht/types.hpp"

#include <array>
#include <chrono>
#include <mutex>
#include <vector>

namespace zeromesh::dht {

// ─────────────────────────────────────────────────────────────────────────────
// Routing Table: 256 k-Buckets indexed by XOR distance bit-prefix
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief A single k-bucket holding up to kBucketSize peer contacts.
 *
 * Peers are ordered from least-recently seen (front) to most-recently seen (back).
 * When full, the Kademlia eviction policy pings the head (least-recently seen)
 * before deciding whether to evict it in favor of the newcomer.
 */
struct KBucket {
    std::vector<DhtNode> entries;
    std::chrono::steady_clock::time_point last_refreshed;
};

/**
 * @brief Thread-safe Kademlia routing table managing 256 k-buckets.
 *
 * Each bucket i stores contacts whose XOR distance from the local node has
 * its highest set bit at position i. The table maintains at most kBucketSize
 * entries per bucket, favoring long-lived nodes per the Kademlia eviction policy.
 *
 * Provides O(1) bucket-index lookup via leading-zero-count on the XOR distance,
 * and O(k × log N) closest-node queries via a sorted merge across adjacent buckets.
 */
class RoutingTable {
public:
    explicit RoutingTable(NodeId self_id);
    ~RoutingTable() = default;

    // Non-copyable
    RoutingTable(const RoutingTable&) = delete;
    RoutingTable& operator=(const RoutingTable&) = delete;

    // ── Core Operations ─────────────────────────────────────────────────

    /**
     * @brief Inserts or refreshes a peer in the appropriate k-bucket.
     *
     * If the target bucket is full and the existing least-recently-seen node
     * is still responsive (determined by the caller via Ping), the new node
     * is placed in the replacement cache instead of evicting the old one.
     *
     * @return true if the node was inserted (or refreshed), false if the
     *         bucket is full and eviction is pending (caller should Ping the
     *         returned stale node to decide).
     */
    bool InsertOrRefresh(const DhtNode& node);

    /**
     * @brief Removes a node from its k-bucket (e.g., after a failed Ping).
     */
    bool Remove(const NodeId& id);

    /**
     * @brief Retrieves the k closest known nodes to a target key.
     *
     * Searches outward from the target's bucket index, collecting entries
     * and sorting by XOR distance. Returns at most @p count results.
     */
    std::vector<DhtNode> FindClosest(const Key& target, size_t count = kBucketSize) const;

    // ── Inspection ──────────────────────────────────────────────────────

    /**
     * @brief Returns the bucket index for a given node ID relative to self.
     *
     * Computes: 255 - clz(self_id ⊕ node_id), where clz counts leading
     * zero bits across the full 256-bit distance.
     *
     * @return Bucket index in [0, 255], or -1 if node_id == self_id.
     */
    [[nodiscard]] int BucketIndexFor(const NodeId& node_id) const;

    /**
     * @brief Returns the local node's identifier.
     */
    [[nodiscard]] const NodeId& GetSelfId() const { return self_id_; }

    /**
     * @brief Returns total number of known peers across all buckets.
     */
    [[nodiscard]] size_t TotalPeerCount() const;

    /**
     * @brief Returns a snapshot of entries in a specific bucket.
     */
    [[nodiscard]] std::vector<DhtNode> GetBucketEntries(int bucket_index) const;

    /**
     * @brief Returns the least-recently-seen node in the given bucket,
     *        or std::nullopt if the bucket is empty.
     */
    [[nodiscard]] std::optional<DhtNode> LeastRecentInBucket(int bucket_index) const;

    /**
     * @brief Returns indices of buckets that have not been refreshed
     *        within the given window, indicating they need a random
     *        FIND_NODE probe to maintain routing health.
     */
    [[nodiscard]] std::vector<int> StaleBuckets(std::chrono::steady_clock::duration max_age) const;

    /**
     * @brief Marks a bucket as freshly refreshed (e.g., after a lookup
     *        that traversed it or after a periodic refresh probe).
     */
    void TouchBucket(int bucket_index);

private:
    NodeId self_id_;
    mutable std::mutex mutex_;
    std::array<KBucket, kMaxBuckets> buckets_;
};

} // namespace zeromesh::dht
