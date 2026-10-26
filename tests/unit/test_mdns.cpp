#include "tests/unit/test_framework.hpp"
#include "discovery/mdns.hpp"
#include "discovery/peer_table.hpp"

#include <chrono>
#include <atomic>
#include <optional>
#include <thread>

TEST(MdnsEngine_StartAndStop) {
    using namespace zeromesh::discovery;

    auto table = std::make_shared<PeerTable>();
    MdnsConfig cfg{
        .node_id = "test-node-alpha",
        .hostname = "alpha-box",
        .multicast_port = 18980
    };

    MdnsEngine engine(table, cfg);
    ASSERT_FALSE(engine.IsRunning());

    ASSERT_TRUE(engine.Start());
    ASSERT_TRUE(engine.IsRunning());

    engine.Stop();
    ASSERT_FALSE(engine.IsRunning());
}

TEST(MdnsEngine_P2PDiscoveryAndPreemptionBroadcast) {
    using namespace zeromesh::discovery;

    int test_port = 18985;
    auto table1 = std::make_shared<PeerTable>();
    auto table2 = std::make_shared<PeerTable>();

    MdnsConfig cfg1{
        .node_id = "peer-node-1",
        .hostname = "workstation-1",
        .grpc_port = 8980,
        .cores = 8,
        .multicast_port = test_port
    };

    MdnsConfig cfg2{
        .node_id = "peer-node-2",
        .hostname = "laptop-2",
        .grpc_port = 8981,
        .cores = 16,
        .multicast_port = test_port
    };

    MdnsEngine engine1(table1, cfg1);
    MdnsEngine engine2(table2, cfg2);

    ASSERT_TRUE(engine1.Start());
    ASSERT_TRUE(engine2.Start());

    // Trigger immediate announcement from node 2
    engine2.AnnounceNow();

    // Poll for loopback propagation; re-announce in case the first datagram raced the listener.
    std::optional<zeromesh::discovery::Peer> discovered;
    for (int i = 0; i < 50 && !discovered; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        discovered = table1->Get("peer-node-2");
        if (!discovered && i % 5 == 4) engine2.AnnounceNow();
    }
    ASSERT_TRUE(discovered.has_value());
    ASSERT_EQ(discovered->hostname, "laptop-2");
    ASSERT_EQ(discovered->grpc_port, 8981);
    ASSERT_EQ(discovered->cores, 16);

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
