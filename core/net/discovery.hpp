#pragma once

// Team-scoped LAN discovery (spec §6.1, §9). Nodes multicast a signed announcement
// every 2 s; only announcements carrying our team id and a valid HMAC tag keyed by
// the team secret are accepted. Without a team the node is silent and deaf.

#include "net/peer_table.hpp"
#include "net/team.hpp"
#include "net/telemetry.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace zeromesh::net {

inline constexpr std::chrono::seconds kAnnounceInterval{2};
inline constexpr std::chrono::seconds kPeerStaleAfter{10};
inline constexpr std::chrono::seconds kPeerPruneAfter{30};
inline constexpr std::chrono::milliseconds kDiscoveryReplaySkew{30000};

// What a node says about itself in one announcement.
struct AnnounceFields {
    std::string node_id;
    std::string boot_id;
    uint64_t seq{0};
    int64_t timestamp_ms{0}; // unix ms; 0 means "use now" when formatting
    std::string hostname;
    int node_port{8981};
    std::string arch;
    Telemetry telemetry;
    PeerState state{PeerState::Active};
};

struct DrainFields {
    std::string node_id;
    std::string boot_id;
    uint64_t seq{0};
    int64_t timestamp_ms{0};
    std::string reason;
};

// ZM3\t<team_id>\t<node_id>\t<boot_id>\t<seq>\t<timestamp_ms>\t<hostname>\t...
//    \t<state>\t<hex mac>
// mac = hex(Mac(key, "zm-announce", everything before the last tab)).
std::string FormatAnnouncement(const AnnounceFields&, const TeamKey&);

struct ParsedAnnouncement {
    Peer peer;
    std::string boot_id;
    uint64_t seq{0};
    int64_t timestamp_ms{0};
};

std::optional<ParsedAnnouncement> ParseAnnouncement(std::string_view packet, const TeamKey&,
                                                   std::string_view self_node_id);

struct DrainNotice {
    std::string node_id;
    std::string reason;
    std::string boot_id;
    uint64_t seq{0};
    int64_t timestamp_ms{0};
};

// ZM3D\t<team_id>\t<node_id>\t<boot_id>\t<seq>\t<timestamp_ms>\t<reason>\t<hex mac>
std::string FormatDrain(const DrainFields&, const TeamKey&);
std::optional<DrainNotice> ParseDrain(std::string_view packet, const TeamKey&,
                                      std::string_view self_node_id);

[[nodiscard]] bool TimestampAcceptable(int64_t unix_ms,
                                       std::chrono::milliseconds max_skew = kDiscoveryReplaySkew);

// Per-sender replay filter: monotonic seq per boot_id; a new boot_id is accepted only
// when the packet timestamp is within the skew window.
class ReplayTracker {
public:
    [[nodiscard]] bool Accept(std::string_view node_id, std::string_view boot_id, uint64_t seq,
                              int64_t timestamp_ms);
    void Clear();

private:
    struct State {
        std::string boot_id;
        uint64_t last_seq{0};
    };
    std::mutex mu_;
    std::unordered_map<std::string, State> nodes_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> skew_log_last_;
};

struct DiscoveryConfig {
    std::string node_id{};
    std::string hostname{};
    int node_port{8981};                     // NodeApi TCP port we announce
    int cores{4};                            // announced when `telemetry` is empty
    uint64_t ram_bytes{16ULL * 1024 * 1024 * 1024};
    int discovery_port{8988};                // UDP; multicast send and listen
    std::string multicast_group{"239.255.42.99"};
    std::optional<TeamKey> team{};           // no team: neither announce nor accept
    std::function<Telemetry()> telemetry{};
};

class Discovery {
public:
    Discovery(std::shared_ptr<PeerTable> peer_table, DiscoveryConfig config);
    ~Discovery();

    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    bool Start();
    void Stop();
    [[nodiscard]] bool IsRunning() const { return running_.load(); }

    void SetTeam(std::optional<TeamKey> team);

    void AnnounceNow();

    void SetLocalState(PeerState state);
    [[nodiscard]] PeerState local_state() const;

    void BroadcastNodeDraining(const std::string& reason);

    [[nodiscard]] const std::string& node_id() const { return config_.node_id; }

private:
    void AdvertiserLoop();
    void ListenerLoop();
    void HandlePacket(std::string_view packet, const std::string& sender_ip);
    void Send(const std::string& msg) const;
    [[nodiscard]] std::optional<TeamKey> Team() const;
    int64_t NowUnixMs() const;
    DrainFields DrainWireFields(std::string_view reason);

    std::shared_ptr<PeerTable> peer_table_;
    DiscoveryConfig config_;
    std::string arch_;
    std::string boot_id_;
    std::atomic<uint64_t> wire_seq_{0};
    ReplayTracker replay_;
    mutable std::mutex team_mutex_;
    std::optional<TeamKey> team_;
    std::mutex telemetry_mutex_;
    mutable std::mutex state_mutex_;
    PeerState local_state_{PeerState::Active};
    std::atomic<bool> running_{false};
    std::thread advertiser_thread_;
    std::thread listener_thread_;
};

} // namespace zeromesh::net
