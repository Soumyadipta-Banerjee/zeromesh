#include "tests/unit/test_framework.hpp"
#include "exec/bwrap.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace {
using Args = std::vector<std::string>;

bool Has(const Args& a, const std::string& flag) {
    return std::find(a.begin(), a.end(), flag) != a.end();
}
size_t Pos(const Args& a, const std::string& flag) {
    return static_cast<size_t>(std::find(a.begin(), a.end(), flag) - a.begin());
}
} // namespace

TEST(BwrapDefaultIsolationFlags) {
    zeromesh::exec::BwrapBuilder b;
    auto args = b.Build({"echo", "hello"});
    ASSERT_EQ(args[0], std::string("bwrap"));
    ASSERT_TRUE(Has(args, "--unshare-all"));
    ASSERT_TRUE(Has(args, "--die-with-parent"));
    ASSERT_TRUE(Has(args, "--clearenv"));
    ASSERT_TRUE(Has(args, "--new-session"));
    ASSERT_FALSE(Has(args, "--share-net"));
    auto sep = Pos(args, "--");
    ASSERT_EQ(args[sep + 1], std::string("echo"));
    ASSERT_EQ(args[sep + 2], std::string("hello"));
}

TEST(BwrapShareNetwork) {
    zeromesh::exec::BwrapBuilder b;
    b.ShareNetwork(true);
    auto args = b.Build({"true"});
    ASSERT_TRUE(Has(args, "--share-net"));
    ASSERT_TRUE(Pos(args, "--share-net") > Pos(args, "--unshare-all"));
}

TEST(BwrapMountOrderPreserved) {
    zeromesh::exec::BwrapBuilder b;
    b.AddReadOnlyBind("/usr", "/usr")
        .AddSymlink("usr/bin", "/bin")
        .AddTmpfs("/tmp")
        .AddReadWriteBind("/scratch/work", "/work")
        .SetBinary("/usr/bin/bwrap");
    auto args = b.Build({"true"});
    ASSERT_EQ(args[0], std::string("/usr/bin/bwrap"));
    auto ro = Pos(args, "--ro-bind");
    auto sym = Pos(args, "--symlink");
    auto tmp = Pos(args, "--tmpfs");
    auto rw = Pos(args, "--bind");
    ASSERT_TRUE(ro < sym && sym < tmp && tmp < rw);
    ASSERT_EQ(args[ro + 1], std::string("/usr"));
    ASSERT_EQ(args[sym + 1], std::string("usr/bin"));
    ASSERT_EQ(args[sym + 2], std::string("/bin"));
    ASSERT_EQ(args[rw + 2], std::string("/work"));
}

TEST(BwrapEnvAndChdir) {
    zeromesh::exec::BwrapBuilder b;
    b.AddEnv("ZM_NODE_ID", "abc").SetWorkingDirectory("/work/sub");
    auto args = b.Build({"true"});
    auto env = Pos(args, "--setenv");
    ASSERT_EQ(args[env + 1], std::string("ZM_NODE_ID"));
    ASSERT_EQ(args[env + 2], std::string("abc"));
    ASSERT_EQ(args[Pos(args, "--chdir") + 1], std::string("/work/sub"));
    ASSERT_TRUE(Pos(args, "--chdir") < Pos(args, "--"));
}

TEST(BwrapPermsDir) {
    zeromesh::exec::BwrapBuilder b;
    b.AddTmpfs("/tmp").AddPerms(0700).AddDir("/tmp/xdg-runtime");
    auto args = b.Build({"true"});
    auto perms = Pos(args, "--perms");
    auto dir = Pos(args, "--dir");
    ASSERT_TRUE(perms < dir);
    ASSERT_EQ(args[perms + 1], std::string("700"));
    ASSERT_EQ(args[dir + 1], std::string("/tmp/xdg-runtime"));
}

namespace {

namespace fs = std::filesystem;

// Writes an executable that prints `banner` for --version, standing in for a bubblewrap build.
std::string FakeBwrap(const fs::path& dir, const std::string& banner, fs::perms extra = fs::perms::none) {
    fs::create_directories(dir);
    const fs::path path = dir / "bwrap";
    std::ofstream(path) << "#!/bin/sh\necho '" << banner << "'\n";
    fs::permissions(path, fs::perms::owner_all | extra);
    return path.string();
}

} // namespace

TEST(BwrapParseVersion) {
    auto v = zeromesh::exec::ParseBwrapVersion("bubblewrap 0.13.0\n");
    ASSERT_TRUE(v.has_value());
    ASSERT_EQ(v->major, 0);
    ASSERT_EQ(v->minor, 13);
    ASSERT_EQ(v->patch, 0);
    ASSERT_TRUE(zeromesh::exec::ParseBwrapVersion("bubblewrap 0.9.1-rc2").has_value());
    ASSERT_FALSE(zeromesh::exec::ParseBwrapVersion("not a version").has_value());
    ASSERT_FALSE(zeromesh::exec::ParseBwrapVersion("").has_value());
}

TEST(BwrapVersionOrdering) {
    using zeromesh::exec::BwrapVersion;
    ASSERT_TRUE((BwrapVersion{0, 12, 0} >= BwrapVersion{0, 12, 0}));
    ASSERT_TRUE((BwrapVersion{0, 13, 0} >= BwrapVersion{0, 12, 0}));
    ASSERT_TRUE((BwrapVersion{1, 0, 0} >= BwrapVersion{0, 12, 9}));
    ASSERT_FALSE((BwrapVersion{0, 11, 9} >= BwrapVersion{0, 12, 0}));
}

TEST(BwrapCheckAcceptsCurrentVersion) {
    const fs::path dir = fs::temp_directory_path() / "zm-bwrap-ok";
    ASSERT_EQ(zeromesh::exec::CheckBwrapBinary(FakeBwrap(dir, "bubblewrap 0.12.0")), std::string());
    fs::remove_all(dir);
}

TEST(BwrapCheckRejectsOldVersion) {
    const fs::path dir = fs::temp_directory_path() / "zm-bwrap-old";
    const std::string why = zeromesh::exec::CheckBwrapBinary(FakeBwrap(dir, "bubblewrap 0.11.0"));
    fs::remove_all(dir);
    ASSERT_FALSE(why.empty());
    ASSERT_TRUE(why.find("0.12.0") != std::string::npos);
}

TEST(BwrapCheckRejectsSetuidBinary) {
    const fs::path dir = fs::temp_directory_path() / "zm-bwrap-setuid";
    const std::string why = zeromesh::exec::CheckBwrapBinary(FakeBwrap(dir, "bubblewrap 0.13.0", fs::perms::set_uid));
    fs::remove_all(dir);
    ASSERT_TRUE(why.find("setuid") != std::string::npos);
}

TEST(BwrapCheckRejectsUnparsableAndMissing) {
    const fs::path dir = fs::temp_directory_path() / "zm-bwrap-junk";
    ASSERT_FALSE(zeromesh::exec::CheckBwrapBinary(FakeBwrap(dir, "something else")).empty());
    fs::remove_all(dir);
    ASSERT_FALSE(zeromesh::exec::CheckBwrapBinary("/nonexistent/bwrap").empty());
}
