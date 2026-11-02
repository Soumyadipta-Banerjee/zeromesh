#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "exec/subprocess.hpp"
#include "net/discovery.hpp"
#include "node/convert.hpp"
#include "node/doctor.hpp"
#include "node/env_capture.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace v1 = zeromesh::v1;
using zeromesh::node::DoctorFacts;
using zeromesh::testing::TempDir;

namespace {

bool Has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

const v1::Finding* Find(const std::vector<v1::Finding>& fs, const std::string& check) {
    for (const auto& f : fs) {
        if (f.check() == check) return &f;
    }
    return nullptr;
}

DoctorFacts Healthy() {
    DoctorFacts f;
    f.overlay = true;
    f.cgroup = "/sys/fs/cgroup/user.slice/zmd";
    f.cgroup_memory = true;
    f.dev_fuse = true;
    f.fusermount = true;
    f.strace = true;
    f.in_team = true;
    f.node_port = 8981;
    f.discovery_port = 8988;
    f.multicast_group = "239.255.42.99";
    f.peers_seen = 2;
    f.peers_pinned = 2;
    f.store_bytes = 1 << 20;
    f.disk_free = 50ull << 30;
    return f;
}

} // namespace

TEST(StraceLogGivesWhatWasReadAndRunAbsolute) {
    const std::string log =
        "140700 execve(\"/usr/bin/ls\", [\"ls\"], 0x7ff /* 88 vars */) = 0\n"
        "140700 openat(AT_FDCWD</work/dir>, \"/etc/ld.so.cache\", O_RDONLY|O_CLOEXEC) = 3</etc/ld.so.cache>\n"
        "140700 openat(AT_FDCWD</work/dir>, \"lib/../share/x.dat\", O_RDONLY) = 3</work/dir/share/x.dat>\n"
        "140700 openat(5</usr/lib/python3>, \"os.py\", O_RDONLY|O_CLOEXEC) = 6</usr/lib/python3/os.py>\n"
        "140700 openat(AT_FDCWD</w>, \"/opt/od\\76d\\\"q\\303\\251/f\", O_RDONLY) = 3</opt/x>\n"
        "140700 openat(AT_FDCWD</w>, \"/usr/lib\", O_RDONLY|O_NONBLOCK|O_CLOEXEC|O_DIRECTORY) = 3</usr/lib>\n"
        "140700 openat(AT_FDCWD</w>, \"/home/u/out.txt\", O_WRONLY|O_CREAT|O_TRUNC, 0666) = 3</home/u/out.txt>\n"
        "140700 openat(AT_FDCWD</w>, \"/home/u/db\", O_RDWR) = 3</home/u/db>\n"
        "140700 openat(AT_FDCWD</w>, \"/etc/ld.so.cache\", O_RDONLY|O_CLOEXEC) = 3</etc/ld.so.cache>\n"
        "140701 execve(\"./configure\", [\"./configure\"], 0x7ff /* 88 vars */) = 0\n"
        "140701 newfstatat(AT_FDCWD</w>, \"/usr/bin/x\", {...}, 0) = 0\n"
        "open(\"/usr/share/zoneinfo/UTC\", O_RDONLY) = 4</usr/share/zoneinfo/UTC>\n";
    const auto t = zeromesh::node::ParseStraceLog(log);
    const std::vector<std::string> opened = {"/etc/ld.so.cache", "/work/dir/share/x.dat", "/usr/lib/python3/os.py",
                                             "/opt/od>d\"q\xc3\xa9/f", "/usr/share/zoneinfo/UTC"};
    ASSERT_EQ(t.opened, opened); // write opens, directories, duplicates and stats are not reads
    ASSERT_EQ(t.executed, std::vector<std::string>{"/usr/bin/ls"}); // ./configure has no directory to resolve against
}

TEST(ExecLoadersFollowShebangAndElfInterpreter) {
    TempDir dir;
    zeromesh::testing::WriteFile(dir / "script", "#!/bin/sh -e\necho hi\n");
    const auto loaders = zeromesh::node::ExecLoaders(dir / "script");
    ASSERT_TRUE(!loaders.empty());
    ASSERT_EQ(loaders[0], std::string("/bin/sh"));
    // This test binary is dynamically linked: its loader is an absolute path that exists.
    const auto self = zeromesh::node::ExecLoaders("/proc/self/exe");
    ASSERT_EQ(self.size(), 1u);
    ASSERT_TRUE(self[0][0] == '/' && fs::exists(self[0]));
    zeromesh::testing::WriteFile(dir / "data", "plain text\n");
    ASSERT_TRUE(zeromesh::node::ExecLoaders(dir / "data").empty());
}

TEST(EnvTreeCarriesOnlyWhatDonorsMount) {
    using zeromesh::node::EnvTreeCarries;
    ASSERT_TRUE(EnvTreeCarries("/usr/bin/env"));
    ASSERT_TRUE(EnvTreeCarries("/opt/blender/blender"));
    ASSERT_TRUE(EnvTreeCarries("/home/u/.local/bin/tool"));
    ASSERT_TRUE(EnvTreeCarries("/etc/ld.so.cache"));
    ASSERT_TRUE(EnvTreeCarries("/etc/ssl/certs/ca.pem"));
    ASSERT_FALSE(EnvTreeCarries("/etc/passwd"));
    ASSERT_FALSE(EnvTreeCarries("/etc/ld.so.cache.d"));
    ASSERT_FALSE(EnvTreeCarries("/tmp/x"));
    ASSERT_FALSE(EnvTreeCarries("/proc/self/maps"));
    ASSERT_FALSE(EnvTreeCarries("/var/lib/x"));
    ASSERT_FALSE(EnvTreeCarries("/"));
    ASSERT_FALSE(EnvTreeCarries("usr/bin/env"));
}

TEST(CaptureTracesAProbeIntoATreeOfAbsolutePaths) {
    if (zeromesh::exec::FindOnPath("strace").empty()) {
        std::cout << " (strace not installed: skipped)";
        return;
    }
    TempDir dir;
    auto cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    auto merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    zeromesh::node::EnvCapturer capturer(merkle, dir / "state");

    const std::string env = zeromesh::exec::FindOnPath("env");
    v1::CaptureRequest req;
    auto* probe = req.add_probes();
    probe->add_argv(env);
    probe->add_argv("true");
    v1::CaptureReply reply;
    auto st = capturer.Capture(req, &reply);
    if (!st.ok()) throw std::runtime_error(st.error_message());
    ASSERT_TRUE(!reply.tree().hash().empty());
    ASSERT_TRUE(reply.files() > 0);
    ASSERT_FALSE(reply.reused());

    const auto root = zeromesh::node::FromProto(reply.tree());
    auto in_tree = [&](const fs::path& p) {
        return merkle->Lookup(root, fs::canonical(p).string().substr(1)).kind ==
               zeromesh::store::TreeEntry::Kind::File;
    };
    ASSERT_TRUE(in_tree(env));
    ASSERT_TRUE(in_tree(zeromesh::exec::FindOnPath("true"))); // executed by env
    const auto loader = zeromesh::node::ExecLoaders(env);
    ASSERT_EQ(loader.size(), 1u);
    ASSERT_TRUE(in_tree(loader[0])); // mapped by the kernel: strace never sees it
    // Only what donors mount from a tree.
    const auto top = merkle->ListNames(root, "").value_or(std::vector<std::string>{});
    for (const char* never : {"proc", "dev", "tmp", "sys", "run", "var"}) ASSERT_FALSE(Has(top, never));
    if (Has(top, "etc")) {
        for (const auto& n : merkle->ListNames(root, "etc").value_or(std::vector<std::string>{})) {
            ASSERT_TRUE(n == "ld.so.cache" || n == "alternatives" || n == "ssl");
        }
    }

    v1::CaptureReply again;
    st = capturer.Capture(req, &again);
    ASSERT_TRUE(st.ok());
    ASSERT_EQ(again.tree().hash(), reply.tree().hash());
    ASSERT_TRUE(again.reused());
}

TEST(CaptureRefusesPathsDonorsNeverMount) {
    TempDir dir;
    auto cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    zeromesh::node::EnvCapturer capturer(std::make_shared<zeromesh::store::MerkleBuilder>(cas), dir / "state");
    for (const char* bad : {"/etc/passwd", "relative/path", "/usr/definitely-not-here-xyz"}) {
        v1::CaptureRequest req;
        req.add_paths(bad);
        v1::CaptureReply reply;
        ASSERT_TRUE(capturer.Capture(req, &reply).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    }
    v1::CaptureRequest none;
    v1::CaptureReply reply;
    ASSERT_TRUE(capturer.Capture(none, &reply).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(CaptureOfAPathAddsItsProgramsLibraries) {
    TempDir dir;
    auto cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    auto merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    zeromesh::node::EnvCapturer capturer(merkle, dir / "state");
    const std::string env = fs::canonical(zeromesh::exec::FindOnPath("env")).string();
    v1::CaptureRequest req;
    req.add_paths(env);
    v1::CaptureReply reply;
    auto st = capturer.Capture(req, &reply);
    if (!st.ok()) throw std::runtime_error(st.error_message());
    const auto root = zeromesh::node::FromProto(reply.tree());
    const fs::path loader = fs::canonical(zeromesh::node::ExecLoaders(env).at(0));
    ASSERT_TRUE(merkle->Lookup(root, loader.string().substr(1)).kind == zeromesh::store::TreeEntry::Kind::File);
    // libc sits beside the loader on Debian, Ubuntu and Arch.
    const auto beside = merkle->ListNames(root, loader.parent_path().string().substr(1)).value_or(std::vector<std::string>{});
    ASSERT_TRUE(std::any_of(beside.begin(), beside.end(), [](const std::string& n) { return n.rfind("libc.so", 0) == 0; }));
}

TEST(DoctorHealthyNodeHasNoWarnings) {
    for (const auto& f : zeromesh::node::Diagnose(Healthy())) {
        if (f.severity() != v1::SEVERITY_OK) throw std::runtime_error(f.check() + ": " + f.message());
    }
}

TEST(DoctorNamesTheFixForEachProblem) {
    auto f = Healthy();
    f.sandbox_problem = "bubblewrap cannot create a sandbox here: bwrap: loopback: Failed RTM_NEWADDR";
    f.apparmor_restricts_userns = 1;
    f.cgroup_memory = false;
    f.dev_fuse = false;
    f.strace = false;
    f.clock_skew_ms["abcdef0123456789"] = 95'000;
    f.disk_free = 100 << 20;
    const auto out = zeromesh::node::Diagnose(f);

    const auto* sandbox = Find(out, "sandbox");
    ASSERT_TRUE(sandbox && sandbox->severity() == v1::SEVERITY_ERROR);
    ASSERT_TRUE(sandbox->fix().find("apparmor_restrict_unprivileged_userns=0") != std::string::npos);
    ASSERT_TRUE(Find(out, "overlay") == nullptr); // meaningless without a sandbox
    ASSERT_TRUE(Find(out, "cgroup")->severity() == v1::SEVERITY_WARN);
    ASSERT_TRUE(Find(out, "fuse")->severity() == v1::SEVERITY_WARN);
    ASSERT_TRUE(Find(out, "strace")->severity() == v1::SEVERITY_WARN);
    const auto* clock = Find(out, "clock");
    ASSERT_TRUE(clock && clock->severity() == v1::SEVERITY_ERROR);
    ASSERT_TRUE(clock->message().find("abcdef01's clock is 95.0 s ahead") != std::string::npos);
    ASSERT_TRUE(Find(out, "store")->severity() == v1::SEVERITY_WARN);

    auto lonely = Healthy();
    lonely.in_team = false;
    lonely.peers_seen = 0;
    lonely.peers_pinned = 0;
    const auto out2 = zeromesh::node::Diagnose(lonely);
    ASSERT_TRUE(Find(out2, "team")->severity() == v1::SEVERITY_WARN);
    ASSERT_TRUE(Find(out2, "discovery") == nullptr); // not in a team: nothing to discover

    auto deaf = Healthy();
    deaf.peers_seen = 0;
    deaf.peers_pinned = 1;
    deaf.discovery_port = 9999;
    const auto out3 = zeromesh::node::Diagnose(deaf);
    const auto* disc = Find(out3, "discovery");
    ASSERT_TRUE(disc && disc->fix().find("UDP 9999") != std::string::npos);
}

TEST(ReplayTrackerRemembersClockSkewUntilItIsFixed) {
    zeromesh::net::ReplayTracker tracker;
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    ASSERT_FALSE(tracker.Accept("node-a", "boot1", 1, now - 120'000));
    const auto skews = tracker.ClockSkews();
    ASSERT_EQ(skews.size(), 1u);
    ASSERT_TRUE(skews.at("node-a") < -100'000);
    ASSERT_TRUE(tracker.Accept("node-a", "boot1", 2, now));
    ASSERT_TRUE(tracker.ClockSkews().empty());
}
