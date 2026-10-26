#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "exec/sandbox_runner.hpp"
#include "plugins/env-host/env_host.hpp"

#include <thread>

namespace {

namespace fs = std::filesystem;
using zeromesh::exec::RunSpec;
using zeromesh::exec::SandboxRunner;
using zeromesh::testing::TempDir;

struct Fixture {
    TempDir dir;
    std::shared_ptr<zeromesh::store::CasStore> cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    std::shared_ptr<zeromesh::store::MerkleBuilder> merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    SandboxRunner runner{cas, merkle, dir / "scratch"};

    zeromesh::exec::RunResult Sh(const std::string& script, RunSpec spec = {}) {
        spec.argv = {"sh", "-c", script};
        spec.sandbox_env = *zeromesh::envhost::Make()->Prepare({}).value;
        return runner.Run(spec, {}, std::stop_token{});
    }
};

} // namespace

TEST(RunnerEchoAndExitCode) {
    Fixture f;
    auto r = f.Sh("echo hi");
    ASSERT_EQ(r.error, std::string());
    ASSERT_EQ(r.exit_code, 0);
    ASSERT_EQ(r.stdout_head, std::string("hi\n"));
    ASSERT_EQ(f.Sh("exit 7").exit_code, 7);
}

TEST(RunnerOutputsDeclaredAndChanged) {
    Fixture f;
    TempDir in;
    zeromesh::testing::WriteFile(in / "in.txt", "input");
    RunSpec spec;
    spec.input_root = f.merkle->BuildTree(in.path());
    spec.outputs = {"out.txt"};
    spec.capture_changed = true;
    auto r = f.Sh("cat in.txt > out.txt; echo y > new.txt", spec);
    ASSERT_EQ(r.exit_code, 0);
    bool out = false, added = false, input = false;
    for (const auto& o : r.outputs) {
        if (o.path == "out.txt") out = *f.cas->GetBlob(o.digest.hash) == "input";
        if (o.path == "new.txt") added = true;
        if (o.path == "in.txt") input = true;
    }
    ASSERT_TRUE(out);
    ASSERT_TRUE(added);
    ASSERT_FALSE(input);
}

TEST(RunnerTimeoutAndCancel) {
    Fixture f;
    RunSpec spec;
    spec.timeout = std::chrono::seconds(1);
    auto r = f.Sh("sleep 30", spec);
    ASSERT_TRUE(r.timed_out);
    ASSERT_TRUE(r.duration < std::chrono::seconds(5));

    std::stop_source src;
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); src.request_stop(); });
    RunSpec s2;
    s2.argv = {"sleep", "30"};
    s2.sandbox_env = *zeromesh::envhost::Make()->Prepare({}).value;
    auto r2 = f.runner.Run(s2, {}, src.get_token());
    t.join();
    ASSERT_TRUE(r2.cancelled);
}

TEST(RunnerIsolation) {
    Fixture f;
    ASSERT_EQ(f.Sh("wc -l < /proc/net/dev").stdout_head, std::string("3\n"));                  // no network
    ASSERT_TRUE(f.Sh("echo z > /usr/zm_test || echo denied").stdout_head.find("denied") != std::string::npos);
    RunSpec spec;
    spec.node_id = "abc";
    ASSERT_EQ(f.Sh("printf %s \"$ZM_NODE_ID\"", spec).stdout_head, std::string("abc"));
    ASSERT_EQ(f.Sh("cut -d' ' -f41 /proc/self/stat").stdout_head, std::string("5\n"));        // SCHED_IDLE
    ASSERT_TRUE(f.Sh("printf %s \"$SSH_AUTH_SOCK\"").stdout_head.empty());                     // env cleared
}

TEST(RunnerCommandNotFoundAndHugeOutput) {
    Fixture f;
    RunSpec spec;
    spec.argv = {"definitely-not-a-binary-xyz"};
    spec.sandbox_env = *zeromesh::envhost::Make()->Prepare({}).value;
    auto r = f.runner.Run(spec, {}, std::stop_token{});
    ASSERT_EQ(r.exit_code, 127);
    ASSERT_FALSE(r.stderr_head.empty());

    auto big = f.Sh("head -c 5000000 /dev/urandom");
    ASSERT_EQ(big.stdout_digest.size_bytes, 5000000);
    ASSERT_EQ(big.stdout_head.size(), zeromesh::exec::kLogHeadBytes);
}

TEST(RunnerOutputSymlinkNotFollowed) {
    Fixture f;
    RunSpec spec;
    spec.outputs = {"out"};
    auto r = f.Sh("ln -s /etc/hostname out", spec);
    ASSERT_EQ(r.outputs.size(), static_cast<size_t>(1));
    ASSERT_TRUE(r.outputs[0].is_symlink);
    ASSERT_EQ(r.outputs[0].symlink_target, std::string("/etc/hostname"));
}
