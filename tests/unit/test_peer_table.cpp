#include "tests/unit/test_framework.hpp"
#include "net/peer_table.hpp"

#include <chrono>

TEST(PeerTable_UpsertAndRetrieve) {
    zeromesh::net::PeerTable table;

    zeromesh::net::Peer p1{
        .id = "node-alpha",
        .hostname = "laptop-1",
        .ip_address = "192.168.1.50",
        .grpc_port = 8980,
        .cores = 8,
        .memory_bytes = 16ULL * 1024 * 1024 * 1024,
        .cpu_load_pct = 15.0,
        .battery_pct = 95,
        .is_charging = true
    };

    table.Upsert(p1);
    ASSERT_EQ(table.Count(), 1);

    auto retrieved = table.Get("node-alpha");
    ASSERT_TRUE(retrieved.has_value());
    ASSERT_EQ(retrieved->hostname, "laptop-1");
    ASSERT_EQ(retrieved->cores, 8);
    ASSERT_EQ(retrieved->endpoint(), "192.168.1.50:8980");

    table.Remove("node-alpha");
    ASSERT_EQ(table.Count(), 0);
    ASSERT_FALSE(table.Get("node-alpha").has_value());
}

TEST(PeerTable_MeasuredLatencySurvivesUpsert) {
    zeromesh::net::PeerTable table;
    zeromesh::net::Peer p{
        .id = "node-beta",
        .hostname = "laptop-2",
        .ip_address = "192.168.1.51",
        .grpc_port = 8980,
        .latency = std::chrono::milliseconds(0),
    };
    table.Upsert(p);
    table.RecordMeasuredLatency("node-beta", std::chrono::milliseconds(42));
    ASSERT_EQ(table.Get("node-beta")->latency, std::chrono::milliseconds(42));

    p.latency = std::chrono::milliseconds(1);  // discovery placeholder
    p.cores = 16;
    table.Upsert(p);
    ASSERT_EQ(table.Get("node-beta")->latency, std::chrono::milliseconds(42));
    ASSERT_EQ(table.Get("node-beta")->cores, 16);
}

TEST(PeerTable_FastPreemptionSignal) {
    zeromesh::net::PeerTable table;

    table.Upsert(zeromesh::net::Peer{
        .id = "node-donor",
        .hostname = "donor-laptop",
        .ip_address = "192.168.1.100",
        .grpc_port = 8980,
        .cores = 8,
        .cpu_load_pct = 20.0,
        .battery_pct = 80,
        .is_charging = true
    });

    bool callback_fired = false;
    std::string drained_id;
    std::string drained_reason;

    table.RegisterPreemptCallback([&](const std::string& id, const std::string& reason) {
        callback_fired = true;
        drained_id = id;
        drained_reason = reason;
    });

    // Trigger fast preemption (e.g. host guard detected low battery or user keyboard activity)
    table.MarkDraining("node-donor", "Battery dropped below 20%");

    ASSERT_TRUE(callback_fired);
    ASSERT_EQ(drained_id, "node-donor");
    ASSERT_EQ(drained_reason, "Battery dropped below 20%");

    // Draining node must no longer be returned as an available active worker
    auto active_nodes = table.ListActive();
    ASSERT_EQ(active_nodes.size(), 0);
}
