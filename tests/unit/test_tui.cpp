#include "tests/unit/test_framework.hpp"
#include "net/peer_table.hpp"
#include "ui/tui.hpp"

#include <iostream>
#include <sstream>

TEST(TerminalUI_PrintStatusSnapshotWithPeers) {
    using namespace zeromesh::net;
    using namespace zeromesh::ui;

    auto peer_table = std::make_shared<PeerTable>();
    peer_table->Upsert(Peer{
        .id = "worker-node-101",
        .hostname = "devbox-alpha",
        .ip_address = "192.168.1.15",
        .grpc_port = 8980,
        .cores = 16,
        .memory_bytes = 32ULL * 1024 * 1024 * 1024,
        .cpu_load_pct = 24.5,
        .battery_pct = 100,
        .is_charging = true,
        .state = PeerState::Active
    });

    peer_table->Upsert(Peer{
        .id = "worker-node-102",
        .hostname = "laptop-beta",
        .ip_address = "192.168.1.25",
        .grpc_port = 8980,
        .cores = 8,
        .memory_bytes = 16ULL * 1024 * 1024 * 1024,
        .cpu_load_pct = 75.0,
        .battery_pct = 18,
        .is_charging = false,
        .state = PeerState::Draining,
        .draining_reason = "Low battery"
    });

    TerminalUI tui(peer_table);

    // Capture standard output
    std::stringstream buffer;
    std::streambuf* prev_cout = std::cout.rdbuf(buffer.rdbuf());

    tui.PrintStatusSnapshot();

    std::cout.rdbuf(prev_cout);
    std::string out = buffer.str();

    ASSERT_TRUE(out.find("ZeroMesh Remote Execution Mesh") != std::string::npos);
    ASSERT_TRUE(out.find("worker-node-101") != std::string::npos);
    ASSERT_TRUE(out.find("16") != std::string::npos);
    ASSERT_TRUE(out.find("32.0 GB") != std::string::npos);
}

TEST(TerminalUI_EmptyClusterHandling) {
    using namespace zeromesh::net;
    using namespace zeromesh::ui;

    auto peer_table = std::make_shared<PeerTable>();
    TerminalUI tui(peer_table);

    std::stringstream buffer;
    std::streambuf* prev_cout = std::cout.rdbuf(buffer.rdbuf());

    tui.PrintStatusSnapshot();

    std::cout.rdbuf(prev_cout);
    std::string out = buffer.str();

    ASSERT_TRUE(out.find("No remote peer nodes discovered yet") != std::string::npos);
    ASSERT_TRUE(out.find("239.255.42.99:8988") != std::string::npos);
}
