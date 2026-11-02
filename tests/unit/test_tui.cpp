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

    ASSERT_TRUE(s.find("0123456789abcdef0123456789abcdef") != std::string::npos); // full self id
    ASSERT_TRUE(s.find("devbox-alpha") != std::string::npos);
    ASSERT_TRUE(s.find("worker-") != std::string::npos); // first 8 chars of peer id
    ASSERT_TRUE(s.find("ADMITTED") != std::string::npos);
    ASSERT_TRUE(s.find("12/16") != std::string::npos);
    ASSERT_TRUE(s.find("32.0 GB") != std::string::npos);
    ASSERT_TRUE(s.find('\033') == std::string::npos); // no colour when asked for none
}

TEST(Tui_NoPeers) {
    zeromesh::v1::PeerList list;
    *list.add_peers() = Peer("0123456789abcdef0123456789abcdef", "devbox-alpha", true);
    std::ostringstream out;
    zeromesh::cli::RenderStatus(list, out, false);
    ASSERT_TRUE(out.str().find("(no peers yet)") != std::string::npos);
}

zeromesh::v1::RunStatus Run(const std::string& id, const std::string& name, int total, int done, int failed,
                            int cached, bool finished) {
    zeromesh::v1::RunStatus r;
    r.set_run_id(id);
    r.set_name(name);
    r.set_total(total);
    r.set_succeeded(done);
    r.set_failed(failed);
    r.set_cached(cached);
    r.set_running(finished ? 0 : total - done - failed);
    r.set_done(finished);
    return r;
}

TEST(Tui_ActivityIdle) {
    zeromesh::v1::ActivitySnapshot snap;
    std::ostringstream out;
    zeromesh::cli::RenderActivity(snap, out, false);
    ASSERT_TRUE(out.str().find("(no runs yet)") != std::string::npos);
}

TEST(Tui_ActivityShowsRunsAndWorkDoneHere) {
    zeromesh::v1::PeerList peers;
    auto* bb = peers.add_peers();
    bb->set_node_id("5768fd74cbf090b439705f55d8552f9d");
    bb->set_hostname("bb");

    zeromesh::v1::ActivitySnapshot snap;
    *snap.add_runs() = Run("a1b2c3d4e5f6", "zm-render", 240, 120, 1, 40, false);
    *snap.add_runs() = Run("0f0f0f0f0f0f", "zm-cc", 10, 10, 0, 10, true);
    auto* b = snap.add_batches();
    b->set_batch("9a/b3");
    b->set_requester("5768fd74cbf090b439705f55d8552f9d");
    b->set_kind("blender");
    b->set_items(8);
    b->set_done(3);
    b->set_current("frame_0012");
    b->set_running(true);

    std::ostringstream out;
    zeromesh::cli::RenderActivity(snap, out, false, &peers);
    const std::string s = out.str();
    ASSERT_TRUE(s.find("zm-render") != std::string::npos);
    ASSERT_TRUE(s.find("121/240") != std::string::npos);
    ASSERT_TRUE(s.find("40 cached") != std::string::npos);
    ASSERT_TRUE(s.find("1 failed") != std::string::npos);
    ASSERT_TRUE(s.find("zm-cc") != std::string::npos);
    ASSERT_TRUE(s.find("done") != std::string::npos);
    ASSERT_TRUE(s.find("Working here") != std::string::npos);
    ASSERT_TRUE(s.find("bb") != std::string::npos); // requester by hostname
    ASSERT_TRUE(s.find("frame_0012") != std::string::npos);
    ASSERT_TRUE(s.find('\033') == std::string::npos);
}

TEST(Tui_MonitorTabsSeparateLiveAndDone) {
    zeromesh::v1::PeerList peers;
    auto* self = peers.add_peers();
    self->set_node_id("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    self->set_hostname("devbox");
    self->set_is_self(true);
    self->set_cores_total(8);
    self->set_cores_free(4);
    zeromesh::v1::ActivitySnapshot snap;
    *snap.add_runs() = Run("live00000000", "running-run", 4, 1, 0, 0, false);
    *snap.add_runs() = Run("done00000000", "finished-run", 4, 4, 0, 0, true);
    std::ostringstream live, done;
    zeromesh::cli::RenderMonitor(peers, snap, live, false, 0, 0, 30, 120);
    zeromesh::cli::RenderMonitor(peers, snap, done, false, 1, 0, 30, 120);
    ASSERT_TRUE(live.str().find("running-run") != std::string::npos);
    ASSERT_TRUE(live.str().find("finished-run") == std::string::npos);
    ASSERT_TRUE(done.str().find("finished-run") != std::string::npos);
    ASSERT_TRUE(done.str().find("running-run") == std::string::npos);
}
