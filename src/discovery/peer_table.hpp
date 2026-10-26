#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeromesh::discovery {

enum class PeerState {
    Active,
    Draining, // Node actively preempting due to low battery, thermal throttle, or user activity
    Offline
};

/**
 * @brief Represents an active ZeroMesh peer discovered on the local network.
 */
struct Peer {
    std::string id;
    std::string hostname;
    std::string ip_address;
    int grpc_port{8980};
    int cores{1};
    uint64_t memory_bytes{0};
    double cpu_load_pct{0.0};     // 0.0 to 100.0%
    int battery_pct{-1};           // -1 for AC/Desktop, 0-100 for battery
    bool is_charging{true};
    PeerState state{PeerState::Active};
    std::string draining_reason{};
    std::chrono::milliseconds latency{0};
    std::chrono::steady_clock::time_point last_seen{};

    [[nodiscard]] std::string endpoint() const {
        return ip_address + ":" + std::to_string(grpc_port);
    }

    [[nodiscard]] bool IsAvailable() const {
        return state == PeerState::Active;
    }
};

/**
 * @brief Thread-safe registry and dynamic routing table of active cluster peers.
 * Supports sub-500ms fast preemption signals (NodeDraining) to immediately re-queue tasks.
 */
class PeerTable {
public:
    using PreemptCallback = std::function<void(const std::string& node_id, const std::string& reason)>;

    PeerTable() = default;
    ~PeerTable() = default;

    // Non-copyable
    PeerTable(const PeerTable&) = delete;
    PeerTable& operator=(const PeerTable&) = delete;

    /**
     * @brief Inserts or updates peer telemetry in the table.
     */
    void Upsert(Peer peer);

    /**
     * @brief Immediately marks a peer as draining and triggers fast preemption callbacks (< 500ms).
     */
    void MarkDraining(const std::string& id, const std::string& reason);

    /**
     * @brief Registers a preemption listener to re-queue tasks immediately when a donor drains.
     */
    void RegisterPreemptCallback(PreemptCallback cb);

    /**
     * @brief Removes a peer by its unique node identifier.
     */
    void Remove(const std::string& id);

    /**
     * @brief Finds a peer by ID.
     */
    [[nodiscard]] std::optional<Peer> Get(const std::string& id) const;

    /**
     * @brief Returns a snapshot of all active peers (excluding draining ones).
     */
    [[nodiscard]] std::vector<Peer> ListActive(std::chrono::milliseconds max_age = std::chrono::seconds(15)) const;

    /**
     * @brief Selects the most capable, least-loaded peer available to accept remote tasks.
     * Evaluates CPU capacity, load, battery, and network latency.
     */
    [[nodiscard]] std::optional<Peer> SelectBestWorker() const;

    /**
     * @brief Removes peers whose last heartbeat exceeded the timeout window.
     */
    size_t PruneStale(std::chrono::milliseconds max_age = std::chrono::seconds(30));

    /**
     * @brief Total registered peer count.
     */
    [[nodiscard]] size_t Count() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Peer> peers_;
    std::vector<PreemptCallback> preempt_callbacks_;
};

} // namespace zeromesh::discovery
