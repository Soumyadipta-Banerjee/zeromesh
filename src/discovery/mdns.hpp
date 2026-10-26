#pragma once

#include "discovery/peer_table.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace zeromesh::discovery {

struct MdnsConfig {
    std::string service_type{"_zeromesh._tcp.local"};
    std::string node_id{};
    std::string hostname{};
    std::string bind_address{"0.0.0.0"};
    int grpc_port{8980};
    int cores{4};
    uint64_t ram_bytes{16ULL * 1024 * 1024 * 1024};
    int multicast_port{8988};
    std::string multicast_group{"239.255.42.99"};
};

/**
 * @brief Zero-configuration peer discovery engine using multicast DNS (mDNS).
 * Broadcasts node capabilities over LAN and continuously listens for peer nodes.
 */
class MdnsEngine {
public:
    MdnsEngine(std::shared_ptr<PeerTable> peer_table, MdnsConfig config);
    ~MdnsEngine();

    // Non-copyable
    MdnsEngine(const MdnsEngine&) = delete;
    MdnsEngine& operator=(const MdnsEngine&) = delete;

    /**
     * @brief Starts the mDNS advertiser and listener threads.
     */
    bool Start();

    /**
     * @brief Stops all mDNS networking threads and unregisters the service.
     */
    void Stop();

    /**
     * @brief Checks if mDNS discovery is currently active.
     */
    [[nodiscard]] bool IsRunning() const { return running_.load(); }

    /**
     * @brief Triggers an immediate mDNS announcement packet broadcast.
     */
    void AnnounceNow();

    /**
     * @brief Broadcasts a high-priority NodeDraining/Preempt multicast packet across the mesh
     * when the host hits battery/thermal limits or interactive user input occurs (< 500ms).
     */
    void BroadcastNodeDraining(const std::string& reason);

private:
    void AdvertiserLoop();
    void ListenerLoop();

    std::shared_ptr<PeerTable> peer_table_;
    MdnsConfig config_;
    std::atomic<bool> running_{false};
    std::thread advertiser_thread_;
    std::thread listener_thread_;
};

} // namespace zeromesh::discovery
