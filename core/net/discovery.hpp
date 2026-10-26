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

namespace zeromesh::net {

inline constexpr std::chrono::seconds kAnnounceInterval{2};
inline constexpr std::chrono::seconds kPeerStaleAfter{10};
inline constexpr std::chrono::seconds kPeerPruneAfter{30};

// What a node says about itself in one announcement.
struct AnnounceFields {
    std::string node_id;
    std::string hostname;
    int node_port{8981};
    std::string arch;
    Telemetry telemetry;
    PeerState state{PeerState::Active};
};

// ZM2\t<team_id>\t<node_id>\t<hostname>\t<node_port>\t<arch>\t<cores_total>\t<cores_free>
//    \t<mem_available>\t<cpu_load_pct>\t<battery_pct>\t<on_ac>\t<state>\t<hex mac>
// mac = hex(Mac(key, "zm-announce", everything before the last tab)).
// Tabs and line breaks in string fields are replaced by spaces.
std::string FormatAnnouncement(const AnnounceFields&, const TeamKey&);

// nullopt for another team, a bad tag, our own node id, or a malformed packet.
// Never throws. ip_address and last_seen are left for the caller to fill in;
// memory_bytes stays 0 (mem_total is not on the wire).
std::optional<Peer> ParseAnnouncement(std::string_view packet, const TeamKey&,
                                      std::string_view self_node_id);

struct DrainNotice {
    std::string node_id;
    std::string reason;
};

// ZM2D\t<team_id>\t<node_id>\t<reason>\t<hex mac>, mac label "zm-drain".
std::string FormatDrain(std::string_view node_id, std::string_view reason, const TeamKey&);
std::optional<DrainNotice> ParseDrain(std::string_view packet, const TeamKey&,
                                      std::string_view self_node_id);

struct DiscoveryConfig {
    std::string node_id{};
    std::string hostname{};
    int node_port{8981};                     // NodeApi TCP port we announce
    int cores{4};                            // announced when `telemetry` is empty
    uint64_t ram_bytes{16ULL * 1024 * 1024 * 1024};
    int discovery_port{8988};                // UDP; multicast send and listen
    std::string multicast_group{"239.255.42.99"};
    std::optional<TeamKey> team{};           // no team: neither announce nor accept
    // Live resource snapshot for announcements. When empty, `cores`/`ram_bytes` are
    // announced with zero load, no battery and AC power. Calls are serialized.
    std::function<Telemetry()> telemetry{};
};

class Discovery {
public:
    Discovery(std::shared_ptr<PeerTable> peer_table, DiscoveryConfig config);
    ~Discovery();

    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;

    // Starts the advertiser and listener threads.
    bool Start();
    void Stop();
    [[nodiscard]] bool IsRunning() const { return running_.load(); }

    // Joins (or with nullopt leaves) a team at runtime. Thread-safe. Changing the team
    // forgets every peer learned under the previous one.
    void SetTeam(std::optional<TeamKey> team);

    // Sends one announcement now. No-op without a team.
    void AnnounceNow();

    // Sticky local peer state for periodic announcements (Active by default).
    // Draining stays on the wire until SetLocalState(Active), so a one-shot
    // BroadcastNodeDraining is not overwritten by the next 2 s announce.
    void SetLocalState(PeerState state);
    [[nodiscard]] PeerState local_state() const;

    // Tells team peers we are draining (battery, thermal, user activity). No-op without a team.
    // Also sets local state to Draining so subsequent AnnounceNow stay draining.
    void BroadcastNodeDraining(const std::string& reason);

    [[nodiscard]] const std::string& node_id() const { return config_.node_id; }

private:
    void AdvertiserLoop();
    void ListenerLoop();
    void HandlePacket(std::string_view packet, const std::string& sender_ip);
    void Send(const std::string& msg) const;
    [[nodiscard]] std::optional<TeamKey> Team() const;

    std::shared_ptr<PeerTable> peer_table_;
    DiscoveryConfig config_;  // config_.team is moved into team_ at construction
    std::string arch_;
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
