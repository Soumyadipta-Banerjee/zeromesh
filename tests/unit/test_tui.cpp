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
    ASSERT_TRUE(s.find("worker-") != std::string::npos);  // first 8 chars of peer id
    ASSERT_TRUE(s.find("ADMITTED") != std::string::npos);
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

zeromesh::v1::ActiveTask Act(const std::string& role, const std::string& name, zeromesh::v1::TaskState state) {
    zeromesh::v1::ActiveTask t;
    t.set_role(role);
    t.set_task_name(name);
    t.set_state(state);
    t.set_job_id("abcdef0123456789");
    t.set_cores(2);
    t.add_argv("blender");
    t.add_argv("-b");
    return t;
}

TEST(Tui_ActivityIdle) {
    zeromesh::v1::ActivitySnapshot snap;
    std::ostringstream out;
    zeromesh::cli::RenderActivity(snap, out, false);
    ASSERT_TRUE(out.str().find("(idle — no tasks yet)") != std::string::npos);
}

TEST(Tui_ActivityKeepsWorkingAndDone) {
    zeromesh::v1::ActivitySnapshot snap;
    *snap.add_tasks() = Act("submitted", "frames_1_8", zeromesh::v1::RUNNING);
    *snap.add_tasks() = Act("submitted", "frames_9_16", zeromesh::v1::SUCCEEDED);
    *snap.add_tasks() = Act("executing", "frames_17_24", zeromesh::v1::FAILED);

    std::ostringstream out;
    zeromesh::cli::RenderActivity(snap, out, false);
    const std::string s = out.str();

    ASSERT_TRUE(s.find("1 working · 1 done · 1 failed/cancel") != std::string::npos);
    ASSERT_TRUE(s.find("WORKING") != std::string::npos);
    ASSERT_TRUE(s.find("DONE") != std::string::npos);
    ASSERT_TRUE(s.find("FAILED") != std::string::npos);
    ASSERT_TRUE(s.find("frames_1_8") != std::string::npos);
    ASSERT_TRUE(s.find("frames_9_16") != std::string::npos);  // finished row stays listed
    ASSERT_TRUE(s.find("submitted") != std::string::npos);
    ASSERT_TRUE(s.find("executing") != std::string::npos);
    ASSERT_TRUE(s.find('\033') == std::string::npos);
}
