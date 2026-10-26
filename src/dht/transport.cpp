#include "dht/transport.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace zeromesh::dht {

// ─────────────────────────────────────────────────────────────────────────────
// Wire Format Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Appends a little-endian uint16 to a byte buffer.
void AppendU16LE(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

/// Appends a little-endian uint32 to a byte buffer.
void AppendU32LE(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

/// Appends a 32-byte NodeId / Key to a byte buffer.
void AppendKey(std::vector<uint8_t>& buf, const Key& key) {
    buf.insert(buf.end(), key.begin(), key.end());
}

/// Appends a length-prefixed string (uint16 LE length + bytes).
void AppendString(std::vector<uint8_t>& buf, const std::string& s) {
    AppendU16LE(buf, static_cast<uint16_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

/// Appends a PeerContact to a byte buffer.
void AppendPeerContact(std::vector<uint8_t>& buf, const PeerContact& pc) {
    AppendKey(buf, pc.id);
    AppendString(buf, pc.ip_address);
    AppendU16LE(buf, pc.port);
}

/// Appends a vector of PeerContacts as [uint16 count][...contacts].
void AppendPeerList(std::vector<uint8_t>& buf, const std::vector<PeerContact>& peers) {
    AppendU16LE(buf, static_cast<uint16_t>(peers.size()));
    for (const auto& pc : peers) {
        AppendPeerContact(buf, pc);
    }
}

/// Serializes the common RPC header.
void AppendHeader(std::vector<uint8_t>& buf, const RpcHeader& h) {
    buf.push_back(static_cast<uint8_t>(h.type));
    AppendU32LE(buf, h.txn_id);
    AppendKey(buf, h.sender_id);
}

// ── Deserialization helpers ─────────────────────────────────────────────────

/// Reader cursor over a raw byte buffer.
struct Reader {
    const uint8_t* data;
    size_t len;
    size_t pos{0};

    bool HasBytes(size_t n) const { return pos + n <= len; }

    uint8_t ReadU8() { return data[pos++]; }

    uint16_t ReadU16LE() {
        uint16_t val = static_cast<uint16_t>(data[pos]) |
                       (static_cast<uint16_t>(data[pos + 1]) << 8);
        pos += 2;
        return val;
    }

    uint32_t ReadU32LE() {
        uint32_t val = static_cast<uint32_t>(data[pos]) |
                       (static_cast<uint32_t>(data[pos + 1]) << 8) |
                       (static_cast<uint32_t>(data[pos + 2]) << 16) |
                       (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return val;
    }

    Key ReadKey() {
        Key k{};
        std::memcpy(k.data(), data + pos, kKeyBytes);
        pos += kKeyBytes;
        return k;
    }

    std::string ReadString() {
        uint16_t slen = ReadU16LE();
        std::string s(reinterpret_cast<const char*>(data + pos), slen);
        pos += slen;
        return s;
    }

    PeerContact ReadPeerContact() {
        PeerContact pc{};
        pc.id = ReadKey();
        pc.ip_address = ReadString();
        pc.port = ReadU16LE();
        return pc;
    }

    std::vector<PeerContact> ReadPeerList() {
        uint16_t count = ReadU16LE();
        std::vector<PeerContact> list;
        list.reserve(count);
        for (uint16_t i = 0; i < count; ++i) {
            list.push_back(ReadPeerContact());
        }
        return list;
    }

    RpcHeader ReadHeader() {
        RpcHeader h{};
        h.type = static_cast<RpcType>(ReadU8());
        h.txn_id = ReadU32LE();
        h.sender_id = ReadKey();
        return h;
    }
};

/// Parses an "ip:port" string into sockaddr_in.
bool ParseEndpoint(const std::string& addr, struct sockaddr_in& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;

    auto colon = addr.rfind(':');
    if (colon == std::string::npos) return false;

    std::string ip = addr.substr(0, colon);
    int port = std::stoi(addr.substr(colon + 1));

    out.sin_port = htons(static_cast<uint16_t>(port));
    return inet_pton(AF_INET, ip.c_str(), &out.sin_addr) == 1;
}

/// Formats a sockaddr_in into an "ip:port" string.
std::string FormatEndpoint(const struct sockaddr_in& addr) {
    char ip_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip_buf, sizeof(ip_buf));
    return std::string(ip_buf) + ":" + std::to_string(ntohs(addr.sin_port));
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// UdpTransport Implementation
// ─────────────────────────────────────────────────────────────────────────────

UdpTransport::UdpTransport(TransportConfig config)
    : config_(std::move(config)) {}

UdpTransport::~UdpTransport() {
    Stop();
}

bool UdpTransport::Start(MessageHandler handler) {
    if (running_.load()) return false;

    handler_ = std::move(handler);

    // Create UDP socket.
    socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) return false;

    // Allow address reuse.
    int opt = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Enable broadcast if configured.
    if (config_.enable_broadcast) {
        setsockopt(socket_fd_, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    }

    // Set buffer sizes.
    int recv_sz = static_cast<int>(config_.recv_buffer_size);
    int send_sz = static_cast<int>(config_.send_buffer_size);
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &recv_sz, sizeof(recv_sz));
    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDBUF, &send_sz, sizeof(send_sz));

    // Bind.
    struct sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(config_.listen_port);
    inet_pton(AF_INET, config_.bind_address.c_str(), &bind_addr.sin_addr);

    if (bind(socket_fd_, reinterpret_cast<struct sockaddr*>(&bind_addr),
             sizeof(bind_addr)) < 0) {
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    running_.store(true);
    recv_thread_ = std::thread(&UdpTransport::ReceiveLoop, this);
    return true;
}

void UdpTransport::Stop() {
    if (!running_.exchange(false)) return;

    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
}

bool UdpTransport::SendMessage(const std::string& dest_addr, const RpcMessage& message) {
    if (socket_fd_ < 0) return false;

    struct sockaddr_in dest{};
    if (!ParseEndpoint(dest_addr, dest)) return false;

    auto buf = Serialize(message);
    ssize_t sent = sendto(socket_fd_, buf.data(), buf.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
    if (sent > 0) {
        packets_sent_.fetch_add(1);
    }
    return sent == static_cast<ssize_t>(buf.size());
}

bool UdpTransport::BroadcastMessage(const RpcMessage& message) {
    if (socket_fd_ < 0) return false;

    struct sockaddr_in bcast{};
    bcast.sin_family = AF_INET;
    bcast.sin_port = htons(config_.listen_port);
    bcast.sin_addr.s_addr = INADDR_BROADCAST;

    auto buf = Serialize(message);
    ssize_t sent = sendto(socket_fd_, buf.data(), buf.size(), 0,
                          reinterpret_cast<struct sockaddr*>(&bcast), sizeof(bcast));
    if (sent > 0) {
        packets_sent_.fetch_add(1);
    }
    return sent == static_cast<ssize_t>(buf.size());
}

std::string UdpTransport::GetLocalEndpoint() const {
    if (socket_fd_ < 0) return "";
    struct sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), &len);
    return FormatEndpoint(addr);
}

void UdpTransport::ReceiveLoop() {
    std::vector<uint8_t> buffer(config_.recv_buffer_size);

    while (running_.load()) {
        struct sockaddr_in sender_addr{};
        socklen_t addr_len = sizeof(sender_addr);

        ssize_t received = recvfrom(socket_fd_, buffer.data(), buffer.size(), 0,
                                    reinterpret_cast<struct sockaddr*>(&sender_addr),
                                    &addr_len);

        if (received <= 0) {
            // Socket closed or error; exit loop if shutting down.
            continue;
        }

        packets_received_.fetch_add(1);

        // Minimum valid packet: 1B type + 4B txn + 32B sender_id = 37 bytes.
        if (received < 37) continue;

        auto msg = Deserialize(buffer.data(), static_cast<size_t>(received));
        if (msg && handler_) {
            std::string sender_ep = FormatEndpoint(sender_addr);
            handler_(sender_ep, *msg);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Serialization
// ─────────────────────────────────────────────────────────────────────────────

std::vector<uint8_t> UdpTransport::Serialize(const RpcMessage& message) {
    std::vector<uint8_t> buf;
    buf.reserve(128);

    std::visit([&buf](const auto& msg) {
        using T = std::decay_t<decltype(msg)>;

        if constexpr (std::is_same_v<T, PingRequest> ||
                      std::is_same_v<T, PongResponse>) {
            AppendHeader(buf, msg.header);
        }
        else if constexpr (std::is_same_v<T, FindNodeRequest>) {
            AppendHeader(buf, msg.header);
            AppendKey(buf, msg.target);
        }
        else if constexpr (std::is_same_v<T, FindNodeResponse>) {
            AppendHeader(buf, msg.header);
            AppendPeerList(buf, msg.closest_nodes);
        }
        else if constexpr (std::is_same_v<T, FindValueRequest>) {
            AppendHeader(buf, msg.header);
            AppendKey(buf, msg.key);
        }
        else if constexpr (std::is_same_v<T, FindValueResponse>) {
            AppendHeader(buf, msg.header);
            // 1 byte flag: 0x01 = value present, 0x00 = nodes only.
            if (msg.value.has_value()) {
                buf.push_back(0x01);
                AppendString(buf, *msg.value);
            } else {
                buf.push_back(0x00);
                AppendPeerList(buf, msg.closest_nodes);
            }
        }
        else if constexpr (std::is_same_v<T, StoreRequest>) {
            AppendHeader(buf, msg.header);
            AppendKey(buf, msg.key);
            AppendString(buf, msg.value);
            AppendU32LE(buf, static_cast<uint32_t>(msg.ttl.count()));
        }
        else if constexpr (std::is_same_v<T, StoreResponse>) {
            AppendHeader(buf, msg.header);
            buf.push_back(msg.success ? 0x01 : 0x00);
        }
        else if constexpr (std::is_same_v<T, AnnounceChunkRequest>) {
            AppendHeader(buf, msg.header);
            AppendKey(buf, msg.chunk_hash);
            AppendU16LE(buf, msg.data_port);
        }
        else if constexpr (std::is_same_v<T, AnnounceChunkResponse>) {
            AppendHeader(buf, msg.header);
            buf.push_back(msg.success ? 0x01 : 0x00);
        }
        else if constexpr (std::is_same_v<T, GetProvidersRequest>) {
            AppendHeader(buf, msg.header);
            AppendKey(buf, msg.chunk_hash);
        }
        else if constexpr (std::is_same_v<T, GetProvidersResponse>) {
            AppendHeader(buf, msg.header);
            AppendPeerList(buf, msg.providers);
            AppendPeerList(buf, msg.closest_nodes);
        }
    }, message);

    return buf;
}

std::optional<RpcMessage> UdpTransport::Deserialize(const uint8_t* data, size_t length) {
    if (length < 37) return std::nullopt; // Header minimum.

    Reader r{data, length};
    RpcHeader header = r.ReadHeader();

    switch (header.type) {
        case RpcType::Ping:
            return PingRequest{header};

        case RpcType::Pong:
            return PongResponse{header};

        case RpcType::FindNode: {
            if (!r.HasBytes(kKeyBytes)) return std::nullopt;
            FindNodeRequest req{header};
            req.target = r.ReadKey();
            return req;
        }

        case RpcType::FindNodeReply: {
            FindNodeResponse resp{header};
            resp.closest_nodes = r.ReadPeerList();
            return resp;
        }

        case RpcType::FindValue: {
            if (!r.HasBytes(kKeyBytes)) return std::nullopt;
            FindValueRequest req{header};
            req.key = r.ReadKey();
            return req;
        }

        case RpcType::FindValueReply: {
            if (!r.HasBytes(1)) return std::nullopt;
            FindValueResponse resp{header};
            uint8_t flag = r.ReadU8();
            if (flag == 0x01) {
                resp.value = r.ReadString();
            } else {
                resp.closest_nodes = r.ReadPeerList();
            }
            return resp;
        }

        case RpcType::Store: {
            if (!r.HasBytes(kKeyBytes)) return std::nullopt;
            StoreRequest req{header};
            req.key = r.ReadKey();
            req.value = r.ReadString();
            req.ttl = std::chrono::seconds(r.ReadU32LE());
            return req;
        }

        case RpcType::StoreReply: {
            if (!r.HasBytes(1)) return std::nullopt;
            StoreResponse resp{header};
            resp.success = (r.ReadU8() == 0x01);
            return resp;
        }

        case RpcType::AnnounceChunk: {
            if (!r.HasBytes(kKeyBytes + 2)) return std::nullopt;
            AnnounceChunkRequest req{header};
            req.chunk_hash = r.ReadKey();
            req.data_port = r.ReadU16LE();
            return req;
        }

        case RpcType::AnnounceReply: {
            if (!r.HasBytes(1)) return std::nullopt;
            AnnounceChunkResponse resp{header};
            resp.success = (r.ReadU8() == 0x01);
            return resp;
        }

        case RpcType::GetProviders: {
            if (!r.HasBytes(kKeyBytes)) return std::nullopt;
            GetProvidersRequest req{header};
            req.chunk_hash = r.ReadKey();
            return req;
        }

        case RpcType::ProvidersReply: {
            GetProvidersResponse resp{header};
            resp.providers = r.ReadPeerList();
            resp.closest_nodes = r.ReadPeerList();
            return resp;
        }
    }

    return std::nullopt; // Unknown RPC type.
}

} // namespace zeromesh::dht
