#pragma once

#include "dht/kademlia.hpp"
#include "dht/types.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace zeromesh::dht {

// ─────────────────────────────────────────────────────────────────────────────
// UDP Transport Layer for DHT Wire Protocol
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Configuration for the DHT UDP transport socket.
 */
struct TransportConfig {
    std::string bind_address{"0.0.0.0"};
    uint16_t listen_port{8982};        ///< Default ZeroMesh DHT port.
    size_t recv_buffer_size{65536};    ///< UDP receive buffer (64 KB).
    size_t send_buffer_size{65536};    ///< UDP send buffer (64 KB).
    bool enable_broadcast{true};       ///< Allow subnet broadcast for LAN bootstrap.
};

/**
 * @brief Callback signature invoked by the transport when a complete
 *        RPC message is received from a remote peer.
 *
 * @param sender_addr IP:port string of the sender (e.g. "192.168.1.42:8982").
 * @param message     The deserialized RPC message variant.
 */
using MessageHandler = std::function<void(const std::string& sender_addr,
                                          const RpcMessage& message)>;

/**
 * @brief Asynchronous UDP transport for DHT message exchange.
 *
 * Manages a non-blocking UDP socket, serializes/deserializes RPC messages
 * using a compact binary wire format, and dispatches incoming messages to
 * a registered handler callback.
 *
 * All DHT packets carry a compact header:
 * ```
 *  [1B type][4B txn_id][32B sender_id][...payload]
 * ```
 *
 * In production deployments, the transport layer encrypts all datagrams
 * with ChaCha20-Poly1305 using the mesh session key (authenticated cluster
 * membership). This base implementation provides the framing; encryption
 * is applied as a composable filter.
 */
class UdpTransport {
public:
    explicit UdpTransport(TransportConfig config = {});
    ~UdpTransport();

    // Non-copyable
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    /**
     * @brief Binds the socket and starts the receive loop thread.
     *
     * @param handler Callback invoked for each received RPC message.
     * @return true on success, false if the socket could not be bound.
     */
    bool Start(MessageHandler handler);

    /**
     * @brief Stops the receive loop and closes the socket.
     */
    void Stop();

    /**
     * @brief Sends a serialized RPC message to the specified peer.
     *
     * @param dest_addr  Destination IP:port (e.g. "192.168.1.10:8982").
     * @param message    The RPC message variant to serialize and send.
     * @return true on successful send, false on socket error.
     */
    bool SendMessage(const std::string& dest_addr, const RpcMessage& message);

    /**
     * @brief Sends a raw broadcast to all peers on the local subnet.
     *
     * Used for LAN bootstrap and fast peer discovery seeding.
     */
    bool BroadcastMessage(const RpcMessage& message);

    /**
     * @brief Returns the local bound address as "ip:port".
     */
    [[nodiscard]] std::string GetLocalEndpoint() const;

    /**
     * @brief Returns whether the transport is actively receiving.
     */
    [[nodiscard]] bool IsRunning() const { return running_.load(); }

    /**
     * @brief Total datagrams sent since start.
     */
    [[nodiscard]] uint64_t GetPacketsSent() const { return packets_sent_.load(); }

    /**
     * @brief Total datagrams received since start.
     */
    [[nodiscard]] uint64_t GetPacketsReceived() const { return packets_received_.load(); }

    // ── Serialization Utilities ─────────────────────────────────────────

    /**
     * @brief Serializes an RpcMessage into a compact binary buffer.
     *
     * Wire format:
     *   [1B RpcType][4B txn_id LE][32B sender_id][variable payload]
     */
    static std::vector<uint8_t> Serialize(const RpcMessage& message);

    /**
     * @brief Deserializes a binary buffer back into an RpcMessage.
     *
     * @return The deserialized message, or std::nullopt on parse failure.
     */
    static std::optional<RpcMessage> Deserialize(const uint8_t* data, size_t length);

private:
    void ReceiveLoop();

    TransportConfig config_;
    int socket_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread recv_thread_;
    MessageHandler handler_;

    std::atomic<uint64_t> packets_sent_{0};
    std::atomic<uint64_t> packets_received_{0};
};

} // namespace zeromesh::dht
