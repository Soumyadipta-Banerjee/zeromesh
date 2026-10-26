#include "tests/unit/test_framework.hpp"
#include "cli/tui.hpp"

#include <sstream>

namespace {

zeromesh::v1::PeerInfo Peer(const std::string& id, const std::string& host, bool self) {
    zeromesh::v1::PeerInfo p;
    p.set_node_id(id);
    p.set_hostname(host);
    p.set_address("192.168.1.15");
    p.set_node_port(8981);
    p.set_cores_total(16);
    p.set_cores_free(12);
    p.set_mem_available(32LL * 1024 * 1024 * 1024);
    p.set_cpu_load_pct(24.5);
    p.set_battery_pct(-1);
    p.set_state("active");
    p.set_is_self(self);
    return p;
}

} // namespace

TEST(Tui_RendersSelfAndPeers) {
    zeromesh::v1::PeerList list;
    *list.add_peers() = Peer("0123456789abcdef0123456789abcdef", "devbox-alpha", true);
    *list.add_peers() = Peer("worker-node-101", "laptop-beta", false);

    std::ostringstream out;
    zeromesh::cli::RenderStatus(list, out, false);
    const std::string s = out.str();

    ASSERT_TRUE(s.find("0123456789abcdef0123456789abcdef") != std::string::npos);  // full self id
    ASSERT_TRUE(s.find("devbox-alpha") != std::string::npos);
    ASSERT_TRUE(s.find("worker-node-101") != std::string::npos);
    ASSERT_TRUE(s.find("12/16") != std::string::npos);
    ASSERT_TRUE(s.find("32.0 GB") != std::string::npos);
    ASSERT_TRUE(s.find('\033') == std::string::npos);  // no colour when asked for none
}

TEST(Tui_NoPeers) {
    zeromesh::v1::PeerList list;
    *list.add_peers() = Peer("0123456789abcdef0123456789abcdef", "devbox-alpha", true);
    std::ostringstream out;
    zeromesh::cli::RenderStatus(list, out, false);
    ASSERT_TRUE(out.str().find("(no peers yet)") != std::string::npos);
}
