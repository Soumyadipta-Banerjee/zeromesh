#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "exec/landlock.hpp"
#include "exec/sandbox_runner.hpp"
#include "plugins/env-host/env_host.hpp"

#include <cstdio>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

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

TEST(RunnerOutputRejectsSymlinkParentEscape) {
    Fixture f;
    TempDir secret;
    zeromesh::testing::WriteFile(secret / "secret", "TOP_SECRET_KEY_MATERIAL");
    const std::string secret_dir = secret.path().string();
    RunSpec spec;
    spec.outputs = {"esc/secret"};
    auto r = f.Sh("ln -s '" + secret_dir + "' esc", spec);
    ASSERT_EQ(r.exit_code, 0);
    for (const auto& o : r.outputs) {
        ASSERT_NE(o.path, std::string("esc/secret"));
        if (!o.digest.hash.empty()) {
            auto blob = f.cas->GetBlob(o.digest.hash);
            ASSERT_TRUE(!blob.has_value() || *blob != "TOP_SECRET_KEY_MATERIAL");
        }
    }

    RunSpec spec2;
    spec2.capture_changed = true;
    auto r2 = f.Sh("ln -s '" + secret_dir + "' esc", spec2);
    ASSERT_EQ(r2.exit_code, 0);
    for (const auto& o : r2.outputs) {
        ASSERT_NE(o.path, std::string("esc/secret"));
    }
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

TEST(RunnerMemoryLimitUnderCap) {
    Fixture f;
    const auto cap = zeromesh::exec::ProbeSandboxResourceEnforcement();
    if (!cap.cgroup_memory && !cap.rlimit_memory) {
        std::fprintf(stderr, "SKIP RunnerMemoryLimitUnderCap: no memory enforcement available\n");
        return;
    }
    RunSpec spec;
    spec.mem_bytes = 64ULL * 1024 * 1024;
    auto r = f.Sh("python3 -c 'b=bytearray(32*1024*1024)'", spec);
    ASSERT_EQ(r.error, std::string());
    ASSERT_EQ(r.exit_code, 0);
    ASSERT_TRUE(r.memory_enforced);
}

TEST(RunnerSigkillNotMisreportedAsOom) {
    Fixture f;
    const auto cap = zeromesh::exec::ProbeSandboxResourceEnforcement();
    if (!cap.cgroup_memory) {
        std::fprintf(stderr, "SKIP RunnerSigkillNotMisreportedAsOom: no cgroup memory enforcement\n");
        return;
    }
    RunSpec spec;
    spec.mem_bytes = 64ULL * 1024 * 1024;
    auto r = f.Sh("kill -9 $$");
    ASSERT_EQ(r.exit_code, 137);
    ASSERT_EQ(r.error, std::string());
}

TEST(RunnerMemoryLimitOverCap) {
    Fixture f;
    const auto cap = zeromesh::exec::ProbeSandboxResourceEnforcement();
    if (!cap.cgroup_memory && !cap.rlimit_memory) {
        std::fprintf(stderr, "SKIP RunnerMemoryLimitOverCap: no memory enforcement available\n");
        return;
    }
    RunSpec spec;
    spec.mem_bytes = 64ULL * 1024 * 1024;
    auto r = f.Sh("python3 -c 'b=bytearray(300*1024*1024)'", spec);
    ASSERT_TRUE(r.memory_enforced);
    if (cap.cgroup_memory) {
        ASSERT_EQ(r.exit_code, 137);
        ASSERT_TRUE(r.error.find("memory limit") != std::string::npos);
    } else {
        ASSERT_NE(r.exit_code, 0);
    }
}

TEST(LandlockRestrictsChildWhenSupported) {
    // Exercises the Landlock module (previously dead code). It must not wrap bwrap;
    // this only checks the LSM ruleset API in a child that never execs bubblewrap.
    if (!zeromesh::exec::LandlockSandbox::IsSupported()) {
        std::fprintf(stderr, "SKIP LandlockRestrictsChildWhenSupported: landlock unavailable\n");
        return;
    }
    TempDir dir;
    zeromesh::testing::WriteFile(dir / "ok.txt", "ok");
    const fs::path ok = dir / "ok.txt";

    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        zeromesh::exec::LandlockSandbox ll;
        ll.AllowReadOnly(dir.path());
        if (!ll.RestrictCurrentProcess()) _exit(2);
        if (access(ok.c_str(), R_OK) != 0) _exit(3);
        _exit(0);
    }
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
}
