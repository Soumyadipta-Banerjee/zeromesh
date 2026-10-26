#include "tests/test_framework.hpp"
#include "dht/kademlia.hpp"
#include "dht/local_store.hpp"
#include "dht/routing_table.hpp"
#include "dht/types.hpp"

TEST(Dht_RoutingTableInsertAndFindClosest) {
    using namespace zeromesh::dht;

    NodeId self_id{};
    self_id.fill(0x01); // 010101...

    RoutingTable table(self_id);
    ASSERT_EQ(table.TotalPeerCount(), 0);

    // Add peer 1
    NodeId peer1_id{};
    peer1_id.fill(0x02);
    DhtNode node1{
        .id = peer1_id,
        .ip_address = "192.168.1.10",
        .port = 8980,
        .last_seen = std::chrono::steady_clock::now(),
        .rtt_us = 500
    };
    ASSERT_TRUE(table.InsertOrRefresh(node1));

    // Add peer 2
    NodeId peer2_id{};
    peer2_id.fill(0xFF);
    DhtNode node2{
        .id = peer2_id,
        .ip_address = "192.168.1.20",
        .port = 8980,
        .last_seen = std::chrono::steady_clock::now(),
        .rtt_us = 600
    };
    ASSERT_TRUE(table.InsertOrRefresh(node2));

    ASSERT_EQ(table.TotalPeerCount(), 2);

    // Find closest to target (target == peer1_id)
    auto closest = table.FindClosest(peer1_id, 2);
    ASSERT_FALSE(closest.empty());
    // node1 should be closest to itself
    ASSERT_EQ(closest[0].id, peer1_id);

    // Remove node
    ASSERT_TRUE(table.Remove(peer1_id));
    ASSERT_EQ(table.TotalPeerCount(), 1);
}

TEST(Dht_LocalStoreValues) {
    using namespace zeromesh::dht;

    LocalStore store;
    Key key{};
    key.fill(0xAA);

    NodeId publisher{};
    publisher.fill(0x10);

    std::string test_data = "dht_action_cache_result_payload";
    ASSERT_TRUE(store.PutValue(key, test_data, publisher, std::chrono::seconds(60)));
    ASSERT_TRUE(store.HasValue(key));

    auto retrieved = store.GetValue(key);
    ASSERT_TRUE(retrieved.has_value());
    ASSERT_EQ(*retrieved, test_data);

    ASSERT_TRUE(store.RemoveValue(key));
    ASSERT_FALSE(store.HasValue(key));
}

TEST(Dht_ProviderRecords) {
    using namespace zeromesh::dht;

    LocalStore store;
    Key chunk_hash{};
    chunk_hash.fill(0xBC);

    PeerContact contact{
        .id = chunk_hash,
        .ip_address = "192.168.1.55",
        .port = 8980
    };
    store.AddProvider(chunk_hash, contact);

    ASSERT_EQ(store.ProviderKeyCount(), 1);

    auto providers = store.GetProviders(chunk_hash);
    ASSERT_EQ(providers.size(), 1);
    ASSERT_EQ(providers[0].ip_address, "192.168.1.55");
    ASSERT_EQ(providers[0].port, 8980);
}
