#pragma once

#include "cas/store.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::cas {

/**
 * @brief Coordinates peer-to-peer chunk replication and fetching of missing CAS blobs over LAN.
 */
class P2PSyncer {
public:
    explicit P2PSyncer(std::shared_ptr<CasStore> store);
    ~P2PSyncer() = default;

    /**
     * @brief Ensures a blob exists locally; if absent, streams it from available LAN peers.
     */
    std::optional<std::string> EnsureBlob(const Digest& digest, const std::vector<std::string>& peer_endpoints);

    /**
     * @brief Advertises a newly generated blob digest to neighboring peers.
     */
    void BroadcastBlobAvailability(const Digest& digest);

    /**
     * @brief Total bytes received over P2P network.
     */
    [[nodiscard]] uint64_t GetTotalBytesReceived() const { return bytes_received_.load(); }

    /**
     * @brief Total chunks synchronized from peers.
     */
    [[nodiscard]] uint64_t GetChunksSynced() const { return chunks_synced_.load(); }

private:
    std::shared_ptr<CasStore> store_;
    mutable std::mutex sync_mutex_;
    std::atomic<uint64_t> bytes_received_{0};
    std::atomic<uint64_t> chunks_synced_{0};
};

} // namespace zeromesh::cas
