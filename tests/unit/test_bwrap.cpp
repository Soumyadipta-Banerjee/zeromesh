#include "tests/unit/test_framework.hpp"
#include "executor/bwrap.hpp"

#include <algorithm>

TEST(BwrapBuilder_DefaultIsolationFlags) {
    zeromesh::executor::BwrapBuilder builder;
    auto args = builder.Build({"echo", "hello"});

    ASSERT_FALSE(args.empty());
    ASSERT_EQ(args[0], "bwrap");

    // Unprivileged namespaces and parent death signal are mandatory
    auto it_unshare = std::find(args.begin(), args.end(), "--unshare-all");
    ASSERT_TRUE(it_unshare != args.end());

    auto it_die = std::find(args.begin(), args.end(), "--die-with-parent");
    ASSERT_TRUE(it_die != args.end());

    // Separator before command
    auto it_sep = std::find(args.begin(), args.end(), "--");
    ASSERT_TRUE(it_sep != args.end());

    // Verify trailing command
    ASSERT_EQ(*(it_sep + 1), "echo");
    ASSERT_EQ(*(it_sep + 2), "hello");
}

TEST(BwrapBuilder_FullHermeticAssembly) {
    zeromesh::executor::BwrapBuilder builder;
    builder.SetCasMaterializedRoot("/cas/sysroot/123")
           .MountEssentialPseudoDevices()
           .MountScratchTmpfs("/tmp")
           .AddReadWriteBind("/host/build", "/workspace")
           .AddEnv("CC", "/usr/bin/clang")
           .SetWorkingDirectory("/workspace");

    auto args = builder.Build({"clang", "-c", "input.c", "-o", "output.o"});

    // Check CAS root bind
    auto it_ro = std::find(args.begin(), args.end(), "--ro-bind");
    ASSERT_TRUE(it_ro != args.end());
    ASSERT_EQ(*(it_ro + 1), "/cas/sysroot/123");
    ASSERT_EQ(*(it_ro + 2), "/");

    // Check /dev and /proc
    auto it_dev = std::find(args.begin(), args.end(), "--dev");
    ASSERT_TRUE(it_dev != args.end());
    ASSERT_EQ(*(it_dev + 1), "/dev");

    auto it_proc = std::find(args.begin(), args.end(), "--proc");
    ASSERT_TRUE(it_proc != args.end());
    ASSERT_EQ(*(it_proc + 1), "/proc");

    // Check tmpfs
    auto it_tmp = std::find(args.begin(), args.end(), "--tmpfs");
    ASSERT_TRUE(it_tmp != args.end());
    ASSERT_EQ(*(it_tmp + 1), "/tmp");

    // Check bind
    auto it_bind = std::find(args.begin(), args.end(), "--bind");
    ASSERT_TRUE(it_bind != args.end());
    ASSERT_EQ(*(it_bind + 1), "/host/build");
    ASSERT_EQ(*(it_bind + 2), "/workspace");

    // Check env
    auto it_env = std::find(args.begin(), args.end(), "--setenv");
    ASSERT_TRUE(it_env != args.end());
    ASSERT_EQ(*(it_env + 1), "CC");
    ASSERT_EQ(*(it_env + 2), "/usr/bin/clang");

    // Check chdir
    auto it_chdir = std::find(args.begin(), args.end(), "--chdir");
    ASSERT_TRUE(it_chdir != args.end());
    ASSERT_EQ(*(it_chdir + 1), "/workspace");

    // Check separator and command
    auto it_sep = std::find(args.begin(), args.end(), "--");
    ASSERT_TRUE(it_sep != args.end());
    ASSERT_EQ(*(it_sep + 1), "clang");
    ASSERT_EQ(*(it_sep + 2), "-c");
    ASSERT_EQ(*(it_sep + 3), "input.c");
    ASSERT_EQ(*(it_sep + 4), "-o");
    ASSERT_EQ(*(it_sep + 5), "output.o");
}
