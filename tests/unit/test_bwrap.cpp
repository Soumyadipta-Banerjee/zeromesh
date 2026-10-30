#include "tests/unit/test_framework.hpp"
#include "exec/bwrap.hpp"

#include <algorithm>

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
