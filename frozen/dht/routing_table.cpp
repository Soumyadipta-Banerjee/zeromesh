#include "dht/routing_table.hpp"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace zeromesh::dht {

// ─────────────────────────────────────────────────────────────────────────────
// Utility: XOR distance and leading-zero-count on 256-bit keys
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Counts the number of leading zero bits in a 256-bit XOR distance.
/// Returns 256 if the distance is zero (i.e. a == b).
int CountLeadingZeros256(const Key& dist) {
    int total_lz = 0;
    for (size_t i = 0; i < kKeyBytes; ++i) {
        if (dist[i] == 0) {
            total_lz += 8;
        } else {
            // __builtin_clz operates on unsigned int (≥32 bits); shift to compensate.
            total_lz += __builtin_clz(static_cast<unsigned int>(dist[i])) - 24;
            break;
        }
    }
    return total_lz;
}

/// Compares two XOR distances as 256-bit big-endian unsigned integers.
/// Returns true if a < b.
bool DistanceLessThan(const Key& a, const Key& b) {
    for (size_t i = 0; i < kKeyBytes; ++i) {
        if (a[i] < b[i]) return true;
        if (a[i] > b[i]) return false;
    }
    return false; // equal
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// RoutingTable Implementation
// ─────────────────────────────────────────────────────────────────────────────

RoutingTable::RoutingTable(NodeId self_id)
    : self_id_(self_id) {
    auto now = std::chrono::steady_clock::now();
    for (auto& bucket : buckets_) {
        bucket.last_refreshed = now;
    }
}

int RoutingTable::BucketIndexFor(const NodeId& node_id) const {
    Key dist = KademliaDht::XorDistance(self_id_, node_id);

    // All-zero distance means the node_id equals self_id_.
    bool all_zero = true;
    for (size_t i = 0; i < kKeyBytes; ++i) {
        if (dist[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) return -1;

    int lz = CountLeadingZeros256(dist);
    // Bucket index: higher index = closer peer.
    // Bucket 255 holds the most distant peers (lz == 0).
    // Bucket 0 holds the closest peers (lz == 255).
    // Convention: bucket_index = 255 - lz.
    return static_cast<int>(kMaxBuckets) - 1 - lz;
}

bool RoutingTable::InsertOrRefresh(const DhtNode& node) {
    int idx = BucketIndexFor(node.id);
    if (idx < 0) return false; // Cannot insert self.

    std::lock_guard<std::mutex> lock(mutex_);
    auto& bucket = buckets_[static_cast<size_t>(idx)];

    // Check if the node already exists in this bucket.
    for (auto& entry : bucket.entries) {
        if (entry.id == node.id) {
            // Refresh: move to tail (most-recently-seen).
            entry.last_seen = node.last_seen;
            entry.ip_address = node.ip_address;
            entry.port = node.port;
            entry.rtt_us = node.rtt_us;

            // Move to back by rotating.
            auto it = std::find_if(bucket.entries.begin(), bucket.entries.end(),
                                   [&](const DhtNode& e) { return e.id == node.id; });
            if (it != bucket.entries.end()) {
                std::rotate(it, it + 1, bucket.entries.end());
            }
            return true;
        }
    }

    // Node not found; insert if bucket has capacity.
    if (bucket.entries.size() < kBucketSize) {
        bucket.entries.push_back(node);
        return true;
    }

    // Bucket full. Per Kademlia spec, the caller should ping the
    // least-recently-seen node (front of the vector) and call
    // Remove() + InsertOrRefresh() if the stale node fails to respond.
    return false;
}

bool RoutingTable::Remove(const NodeId& id) {
    int idx = BucketIndexFor(id);
    if (idx < 0) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto& bucket = buckets_[static_cast<size_t>(idx)];

    auto it = std::remove_if(bucket.entries.begin(), bucket.entries.end(),
                             [&](const DhtNode& e) { return e.id == id; });
    if (it == bucket.entries.end()) return false;

    bucket.entries.erase(it, bucket.entries.end());
    return true;
}

std::vector<DhtNode> RoutingTable::FindClosest(const Key& target, size_t count) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // Collect all known peers from all buckets.
    std::vector<DhtNode> candidates;
    for (const auto& bucket : buckets_) {
        for (const auto& entry : bucket.entries) {
            candidates.push_back(entry);
        }
    }

    // Sort by XOR distance to the target.
    std::sort(candidates.begin(), candidates.end(),
              [&target](const DhtNode& a, const DhtNode& b) {
                  Key dist_a = KademliaDht::XorDistance(a.id, target);
                  Key dist_b = KademliaDht::XorDistance(b.id, target);
                  return DistanceLessThan(dist_a, dist_b);
              });

    // Return the closest `count` nodes.
    if (candidates.size() > count) {
        candidates.resize(count);
    }
    return candidates;
}

size_t RoutingTable::TotalPeerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = 0;
    for (const auto& bucket : buckets_) {
        total += bucket.entries.size();
    }
    return total;
}

std::vector<DhtNode> RoutingTable::GetBucketEntries(int bucket_index) const {
    if (bucket_index < 0 || bucket_index >= static_cast<int>(kMaxBuckets)) {
        return {};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return buckets_[static_cast<size_t>(bucket_index)].entries;
}

std::optional<DhtNode> RoutingTable::LeastRecentInBucket(int bucket_index) const {
    if (bucket_index < 0 || bucket_index >= static_cast<int>(kMaxBuckets)) {
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& entries = buckets_[static_cast<size_t>(bucket_index)].entries;
    if (entries.empty()) return std::nullopt;
    return entries.front(); // Front is least-recently-seen.
}

std::vector<int> RoutingTable::StaleBuckets(std::chrono::steady_clock::duration max_age) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    std::vector<int> stale;

    for (int i = 0; i < static_cast<int>(kMaxBuckets); ++i) {
        if (!buckets_[static_cast<size_t>(i)].entries.empty() &&
            (now - buckets_[static_cast<size_t>(i)].last_refreshed) > max_age) {
            stale.push_back(i);
        }
    }
    return stale;
}

void RoutingTable::TouchBucket(int bucket_index) {
    if (bucket_index < 0 || bucket_index >= static_cast<int>(kMaxBuckets)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    buckets_[static_cast<size_t>(bucket_index)].last_refreshed = std::chrono::steady_clock::now();
}

} // namespace zeromesh::dht
