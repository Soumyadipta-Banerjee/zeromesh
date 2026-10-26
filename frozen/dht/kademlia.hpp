#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::dht {

constexpr size_t kKeyBytes = 32;       // 256 bits (BLAKE3)
constexpr size_t kBucketSize = 8;      // k-bucket capacity for LAN mesh
constexpr size_t kMaxBuckets = 256;    // 256-bit identifier space

using NodeId = std::array<uint8_t, kKeyBytes>;
using Key = std::array<uint8_t, kKeyBytes>;

/**
 * @brief Representation of an active node in the DHT routing table.
 */
struct DhtNode {
    NodeId id{};
    std::string ip_address;
    uint16_t port{0};
    std::chrono::steady_clock::time_point last_seen;
    uint32_t rtt_us{0};
};

/**
 * @brief Provider record indicating which nodes possess a specific CAS chunk.
 */
struct ProviderRecord {
    Key chunk_hash{};
    std::vector<DhtNode> providers;
    std::chrono::steady_clock::time_point expires_at;
};

/**
 * @brief BitTorrent-style Kademlia Distributed Hash Table (DHT).
 * Coordinates chunk provider discovery (ANNOUNCE_CHUNK/GET_PROVIDERS),
 * Action Cache memoization, and peer routing without a central tracker.
 */
class KademliaDht {
public:
    explicit KademliaDht(NodeId self_id, uint16_t listen_port = 8982);
    ~KademliaDht();

    // Non-copyable
    KademliaDht(const KademliaDht&) = delete;
    KademliaDht& operator=(const KademliaDht&) = delete;

    /**
     * @brief Computes XOR distance between two 256-bit identifiers.
     */
    static Key XorDistance(const Key& a, const Key& b);

    /**
     * @brief Starts the UDP listener and routing loop.
     */
    bool Start();

    /**
     * @brief Stops the DHT engine.
     */
    void Stop();

    /**
     * @brief Adds or updates a known peer in the k-buckets.
     */
    void InsertNode(const DhtNode& node);

    /**
     * @brief Finds the k-closest nodes to a target key.
     */
    std::vector<DhtNode> FindClosestNodes(const Key& target, size_t count = kBucketSize);

    /**
     * @brief Announces that the local node holds a specific CAS chunk.
     * @param chunk_hash  256-bit hash of the chunk.
     * @param data_port   Optional TCP data port where this node serves chunks (0 = defaults to listen_port).
     */
    void AnnounceChunk(const Key& chunk_hash, uint16_t data_port = 0);

    /**
     * @brief Queries the DHT for peers holding a specific CAS chunk.
     */
    std::vector<DhtNode> GetChunkProviders(const Key& chunk_hash);

    /**
     * @brief Stores an arbitrary key-value entry (e.g. Action Cache result) in the DHT.
     */
    void StoreValue(const Key& key, const std::string& value);

    /**
     * @brief Directly sends a StoreRequest for key/value to a specific peer endpoint.
     */
    void StoreValueDirect(const std::string& ip, uint16_t port, const Key& key, const std::string& value);

    /**
     * @brief Broadcasts a StoreRequest on the local subnet.
     */
    void BroadcastStore(const Key& key, const std::string& value);

    /**
     * @brief Retrieves a key-value entry from the DHT.
     */
    std::optional<std::string> FindValue(const Key& key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeromesh::dht
