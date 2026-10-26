#include "tests/unit/test_framework.hpp"
#include "net/discovery.hpp"
#include "net/peer_table.hpp"

#include <chrono>
#include <atomic>
#include <optional>
#include <thread>

TEST(Discovery_StartAndStop) {
    using namespace zeromesh::net;

    auto table = std::make_shared<PeerTable>();
    DiscoveryConfig cfg{
        .node_id = "test-node-alpha",
        .hostname = "alpha-box",
        .multicast_port = 18980
    };

    Discovery engine(table, cfg);
    ASSERT_FALSE(engine.IsRunning());

    ASSERT_TRUE(engine.Start());
    ASSERT_TRUE(engine.IsRunning());

    engine.Stop();
    ASSERT_FALSE(engine.IsRunning());
}

TEST(Discovery_P2PAnnounceTelemetryAndDrain) {
    using namespace zeromesh::net;

    int test_port = 18985;
    auto table1 = std::make_shared<PeerTable>();
    auto table2 = std::make_shared<PeerTable>();

    DiscoveryConfig cfg1{
        .node_id = "peer-node-1",
        .hostname = "workstation-1",
        .grpc_port = 8980,
        .cores = 8,
        .multicast_port = test_port
    };

    DiscoveryConfig cfg2{
        .node_id = "peer-node-2",
        .hostname = "laptop-2",
        .grpc_port = 8981,
        .node_port = 18981,
        .cores = 16,
        .multicast_port = test_port,
        .telemetry = [] {
            return Telemetry{.cores_total = 16, .cores_free = 12, .mem_total = 1000,
                             .mem_available = 600, .cpu_load_pct = 25.0, .battery_pct = 42,
                             .on_ac = false, .arch = "riscv64"};
        }
    };

    Discovery engine1(table1, cfg1);
    Discovery engine2(table2, cfg2);

    ASSERT_TRUE(engine1.Start());
    ASSERT_TRUE(engine2.Start());

    // Trigger immediate announcement from node 2
    engine2.AnnounceNow();

    // Poll for loopback propagation; re-announce in case the first datagram raced the listener.
    std::optional<zeromesh::net::Peer> discovered;
    for (int i = 0; i < 50 && !discovered; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        discovered = table1->Get("peer-node-2");
        if (!discovered && i % 5 == 4) engine2.AnnounceNow();
    }
    ASSERT_TRUE(discovered.has_value());
    ASSERT_EQ(discovered->hostname, "laptop-2");
    ASSERT_EQ(discovered->grpc_port, 8981);
    ASSERT_EQ(discovered->cores, 16);
    ASSERT_EQ(discovered->cores_free, 12);
    ASSERT_EQ(discovered->mem_available, 600u);
    ASSERT_EQ(discovered->battery_pct, 42);
    ASSERT_FALSE(discovered->is_charging);
    ASSERT_EQ(discovered->arch, "riscv64");
    ASSERT_EQ(discovered->node_port, 18981);

    // Test fast preemption signal propagation (< 500ms)
    std::atomic<bool> preempt_received{false};
    std::string preempt_reason;
    table1->RegisterPreemptCallback([&](const std::string& id, const std::string& reason) {
        if (id == "peer-node-2") {
            preempt_received = true;
            preempt_reason = reason;
        }
    });

    engine2.BroadcastNodeDraining("User opened IDE on donor laptop");

    for (int i = 0; i < 50 && !preempt_received; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ASSERT_TRUE(preempt_received);
    ASSERT_EQ(preempt_reason, "User opened IDE on donor laptop");

    engine1.Stop();
    engine2.Stop();
}
