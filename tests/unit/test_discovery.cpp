#include "tests/unit/test_framework.hpp"
#include "net/discovery.hpp"
#include "net/peer_table.hpp"
#include "net/team.hpp"

#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <thread>

using namespace zeromesh::net;

namespace {

AnnounceFields SampleFields(uint64_t seq = 1, int64_t timestamp_ms = 0) {
    AnnounceFields a{
        .node_id = "node-a",
        .boot_id = "0123456789abcdef0123456789abcdef",
        .seq = seq,
        .timestamp_ms = timestamp_ms,
        .hostname = "box\twith\ntabs",
        .node_port = 18981,
        .arch = "riscv64",
        .telemetry = Telemetry{.cores_total = 16, .cores_free = 12, .mem_total = 1000,
                               .mem_available = 600, .cpu_load_pct = 25.5, .battery_pct = 42,
                               .on_ac = false, .arch = "riscv64"},
    };
    if (a.timestamp_ms == 0) {
        a.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    }
    return a;
}

DrainFields SampleDrain(uint64_t seq = 1, int64_t timestamp_ms = 0) {
    DrainFields d{
        .node_id = "node-a",
        .boot_id = "0123456789abcdef0123456789abcdef",
        .seq = seq,
        .timestamp_ms = timestamp_ms,
        .reason = "user\tis back",
    };
    if (d.timestamp_ms == 0) {
        d.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    }
    return d;
}

int RandomHighPort() {
    std::random_device rd;
    return 20000 + static_cast<int>(rd() % 40000);
}

DiscoveryConfig LoopbackConfig(const std::string& id, int port, const TeamKey& key) {
    DiscoveryConfig cfg;
    cfg.node_id = id;
    cfg.hostname = id + "-host";
    cfg.discovery_port = port;
    cfg.team = key;
    return cfg;
}

} // namespace

TEST(AnnounceRoundTrip) {
    const TeamKey key = GenerateTeamKey();
    const auto packet = FormatAnnouncement(SampleFields(), key);
    ASSERT_TRUE(packet.starts_with("ZM3\t" + TeamId(key) + "\t"));

    const auto parsed = ParseAnnouncement(packet, key, "self");
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->peer.id, "node-a");
    ASSERT_EQ(parsed->peer.hostname, "box with tabs");
    ASSERT_EQ(parsed->peer.node_port, 18981);
    ASSERT_EQ(parsed->peer.arch, "riscv64");
    ASSERT_EQ(parsed->peer.cores, 16);
    ASSERT_EQ(parsed->peer.cores_free, 12);
    ASSERT_EQ(parsed->peer.mem_available, 600u);
    ASSERT_EQ(parsed->peer.cpu_load_pct, 25.5);
    ASSERT_EQ(parsed->peer.battery_pct, 42);
    ASSERT_FALSE(parsed->peer.is_charging);
    ASSERT_TRUE(parsed->peer.state == PeerState::Active);
}

TEST(AnnounceOtherTeamIgnored) {
    const auto packet = FormatAnnouncement(SampleFields(), GenerateTeamKey());
    ASSERT_FALSE(ParseAnnouncement(packet, GenerateTeamKey(), "self").has_value());
}

TEST(AnnounceTamperedRejected) {
    const TeamKey key = GenerateTeamKey();
    auto packet = FormatAnnouncement(SampleFields(), key);
    const auto pos = packet.find("\t18981\t");
    ASSERT_TRUE(pos != std::string::npos);
    packet[pos + 1] = '2';
    ASSERT_FALSE(ParseAnnouncement(packet, key, "self").has_value());
    ASSERT_FALSE(ParseAnnouncement("ZM3\tgarbage", key, "self").has_value());
}

TEST(AnnounceSelfIgnored) {
    const TeamKey key = GenerateTeamKey();
    const auto packet = FormatAnnouncement(SampleFields(), key);
    ASSERT_FALSE(ParseAnnouncement(packet, key, "node-a").has_value());
}

TEST(DrainRoundTrip) {
    const TeamKey key = GenerateTeamKey();
    const auto packet = FormatDrain(SampleDrain(), key);
    const auto drain = ParseDrain(packet, key, "self");
    ASSERT_TRUE(drain.has_value());
    ASSERT_EQ(drain->node_id, "node-a");
    ASSERT_EQ(drain->reason, "user is back");
    ASSERT_FALSE(ParseDrain(packet, GenerateTeamKey(), "self").has_value());
    ASSERT_FALSE(ParseDrain(packet, key, "node-a").has_value());
}

TEST(ReplayTrackerRejectsReplayedSeq) {
    ReplayTracker tracker;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    ASSERT_TRUE(tracker.Accept("node-a", "boot1", 1, now));
    ASSERT_FALSE(tracker.Accept("node-a", "boot1", 1, now));
    ASSERT_TRUE(tracker.Accept("node-a", "boot1", 2, now));
}

TEST(ReplayTrackerRejectsStaleTimestamp) {
    ReplayTracker tracker;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    ASSERT_FALSE(tracker.Accept("node-a", "boot1", 1, now - 60'000));
    ASSERT_FALSE(TimestampAcceptable(now - 60'000));
    ASSERT_TRUE(TimestampAcceptable(now));
}

TEST(ReplayTrackerAcceptsNewBootIdWithFreshTimestamp) {
    ReplayTracker tracker;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    ASSERT_TRUE(tracker.Accept("node-a", "boot1", 5, now));
    ASSERT_TRUE(tracker.Accept("node-a", "boot2", 1, now));
}

TEST(DiscoveryLoopbackSameTeam) {
    const int port = RandomHighPort();
    const TeamKey key = GenerateTeamKey();
    auto table_a = std::make_shared<PeerTable>();
    auto table_b = std::make_shared<PeerTable>();
    Discovery a(table_a, LoopbackConfig("loop-a", port, key));
    Discovery b(table_b, LoopbackConfig("loop-b", port, key));
    ASSERT_TRUE(a.Start());
    ASSERT_TRUE(b.Start());

    bool seen = false;
    for (int i = 0; i < 50 && !seen; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        seen = table_a->Get("loop-b").has_value() && table_b->Get("loop-a").has_value();
    }
    ASSERT_TRUE(seen);
    const auto b_seen_by_a = table_a->Get("loop-b");
    ASSERT_EQ(b_seen_by_a->hostname, "loop-b-host");
    ASSERT_FALSE(b_seen_by_a->ip_address.empty());
}

TEST(DiscoveryLoopbackOtherTeam) {
    const int port = RandomHighPort();
    auto table_a = std::make_shared<PeerTable>();
    auto table_b = std::make_shared<PeerTable>();
    Discovery a(table_a, LoopbackConfig("other-a", port, GenerateTeamKey()));
    Discovery b(table_b, LoopbackConfig("other-b", port, GenerateTeamKey()));
    ASSERT_TRUE(a.Start());
    ASSERT_TRUE(b.Start());

    std::this_thread::sleep_for(std::chrono::seconds(3));
    ASSERT_EQ(table_a->Count(), 0u);
    ASSERT_EQ(table_b->Count(), 0u);
}
