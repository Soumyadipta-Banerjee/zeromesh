#pragma once

#include "dht/kademlia.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace zeromesh::dht {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration Constants
// ─────────────────────────────────────────────────────────────────────────────

/// Concurrency parameter α: max parallel in-flight RPCs per iterative lookup.
constexpr size_t kAlpha = 3;

/// Default replication factor: number of closest nodes to store a value on.
constexpr size_t kReplicationFactor = 3;

/// Default TTL for provider records before automatic expiry.
constexpr auto kProviderTtl = std::chrono::minutes(30);

/// Default TTL for arbitrary key-value entries.
constexpr auto kValueTtl = std::chrono::hours(1);

/// Maximum RPC round-trip timeout before declaring a peer unresponsive.
constexpr auto kRpcTimeout = std::chrono::milliseconds(2000);

/// Interval between periodic k-bucket refresh sweeps.
constexpr auto kRefreshInterval = std::chrono::minutes(15);

/// Maximum byte size of a DHT value payload (Action Cache entries, DSM leases).
constexpr size_t kMaxValueSize = 8192;

// ─────────────────────────────────────────────────────────────────────────────
// RPC Message Type Enumeration
// ─────────────────────────────────────────────────────────────────────────────

enum class RpcType : uint8_t {
    Ping           = 0x01,
    Pong           = 0x02,
    FindNode       = 0x10,
    FindNodeReply  = 0x11,
    FindValue      = 0x20,
    FindValueReply = 0x21,
    Store          = 0x30,
    StoreReply     = 0x31,
    AnnounceChunk  = 0x40,
    AnnounceReply  = 0x41,
    GetProviders   = 0x50,
    ProvidersReply = 0x51
};

// ─────────────────────────────────────────────────────────────────────────────
// RPC Request / Response Payload Structs
// ─────────────────────────────────────────────────────────────────────────────

/// Compact peer contact info serialized on the wire.
struct PeerContact {
    NodeId id{};
    std::string ip_address;
    uint16_t port{0};
};

/// Transaction nonce to match requests to responses.
using TransactionId = uint32_t;

/// Common header present on every DHT datagram.
struct RpcHeader {
    RpcType type{};
    TransactionId txn_id{0};
    NodeId sender_id{};
};

// ── PING / PONG ─────────────────────────────────────────────────────────────

struct PingRequest {
    RpcHeader header;
};

struct PongResponse {
    RpcHeader header;
};

// ── FIND_NODE ───────────────────────────────────────────────────────────────

struct FindNodeRequest {
    RpcHeader header;
    Key target{};
};

struct FindNodeResponse {
    RpcHeader header;
    std::vector<PeerContact> closest_nodes;
};

// ── FIND_VALUE ──────────────────────────────────────────────────────────────

struct FindValueRequest {
    RpcHeader header;
    Key key{};
};

struct FindValueResponse {
    RpcHeader header;
    /// If the value was found locally, it is returned here.
    std::optional<std::string> value;
    /// Otherwise, the k closest known nodes are returned for further lookup.
    std::vector<PeerContact> closest_nodes;
};

// ── STORE ───────────────────────────────────────────────────────────────────

struct StoreRequest {
    RpcHeader header;
    Key key{};
    std::string value;
    std::chrono::seconds ttl{3600};
};

struct StoreResponse {
    RpcHeader header;
    bool success{false};
};

// ── ANNOUNCE_CHUNK / GET_PROVIDERS ──────────────────────────────────────────

struct AnnounceChunkRequest {
    RpcHeader header;
    Key chunk_hash{};
    uint16_t data_port{0}; ///< Port on which the sender serves chunk data.
};

struct AnnounceChunkResponse {
    RpcHeader header;
    bool success{false};
};

struct GetProvidersRequest {
    RpcHeader header;
    Key chunk_hash{};
};

struct GetProvidersResponse {
    RpcHeader header;
    std::vector<PeerContact> providers;
    /// Additional closest nodes for continued iterative search.
    std::vector<PeerContact> closest_nodes;
};

// ── Variant-based polymorphic message container ─────────────────────────────

using RpcMessage = std::variant<
    PingRequest,
    PongResponse,
    FindNodeRequest,
    FindNodeResponse,
    FindValueRequest,
    FindValueResponse,
    StoreRequest,
    StoreResponse,
    AnnounceChunkRequest,
    AnnounceChunkResponse,
    GetProvidersRequest,
    GetProvidersResponse
>;

} // namespace zeromesh::dht
