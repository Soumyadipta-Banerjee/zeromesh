#include "tests/unit/test_framework.hpp"
#include "discovery/peer_table.hpp"

#include <chrono>

TEST(PeerTable_UpsertAndRetrieve) {
    zeromesh::discovery::PeerTable table;

    zeromesh::discovery::Peer p1{
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

TEST(PeerTable_SelectBestWorker) {
    zeromesh::discovery::PeerTable table;

    // Node 1: 4 cores, 80% load
    table.Upsert(zeromesh::discovery::Peer{
        .id = "node-busy",
        .hostname = "busy-pc",
        .ip_address = "192.168.1.10",
        .grpc_port = 8980,
        .cores = 4,
        .cpu_load_pct = 80.0,
        .battery_pct = 100,
        .is_charging = true
    });

    // Node 2: 8 cores, 10% load (highest compute capacity)
    table.Upsert(zeromesh::discovery::Peer{
        .id = "node-fast",
        .hostname = "fast-workstation",
        .ip_address = "192.168.1.20",
        .grpc_port = 8980,
        .cores = 8,
        .cpu_load_pct = 10.0,
        .battery_pct = 100,
        .is_charging = true
    });

    // Node 3: 16 cores, but battery < 20% and not charging (should be protected and not chosen)
    table.Upsert(zeromesh::discovery::Peer{
        .id = "node-low-battery",
        .hostname = "dying-laptop",
        .ip_address = "192.168.1.30",
        .grpc_port = 8980,
        .cores = 16,
        .cpu_load_pct = 5.0,
        .battery_pct = 12,
        .is_charging = false
    });

    auto best = table.SelectBestWorker();
    ASSERT_TRUE(best.has_value());
    // Must select node-fast, skipping dying-laptop due to battery guard
    ASSERT_EQ(best->id, "node-fast");
}

TEST(PeerTable_FastPreemptionSignal) {
    zeromesh::discovery::PeerTable table;

    table.Upsert(zeromesh::discovery::Peer{
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
    ASSERT_FALSE(table.SelectBestWorker().has_value());
}
