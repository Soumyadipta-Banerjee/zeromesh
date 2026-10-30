#include "tests/unit/test_framework.hpp"
#include "plugin/loader.hpp"
#include "plugins/env-host/env_host.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using zeromesh::envhost::Resolve;

} // namespace

TEST(EnvHostResolvesProgramsOnTheSandboxPath) {
    auto env = zeromesh::envhost::Make();
    ASSERT_EQ(env->Name(), std::string_view("host"));
    ASSERT_TRUE(env->Check({}).ok); // programs are the core's business, not the provider's
    auto sh = Resolve("sh");
    ASSERT_TRUE(sh.has_value());
    ASSERT_TRUE(sh->rfind("/", 0) == 0);
    ASSERT_FALSE(Resolve("definitely-not-a-binary-xyz").has_value());
    ASSERT_FALSE(Resolve("").has_value());
    ASSERT_FALSE(Resolve("bin/sim").has_value()); // relative: lives in the workspace
}

TEST(EnvHostMirrorsUsrMerge) {
    auto r = zeromesh::envhost::Make()->Prepare({});
    ASSERT_TRUE(r.value.has_value());
    const auto& env = *r.value;
    for (const char* p : {"/bin", "/lib"}) {
        bool sym = std::any_of(env.symlinks.begin(), env.symlinks.end(), [&](const auto& s) { return s.link == p; });
        bool bind = std::any_of(env.binds.begin(), env.binds.end(),
                                [&](const auto& b) { return b.sandbox == p && b.read_only; });
        if (fs::is_symlink(p)) {
            ASSERT_TRUE(sym && !bind);
        } else {
            ASSERT_TRUE(bind && !sym);
        }
    }
    ASSERT_TRUE(std::any_of(env.env.begin(), env.env.end(), [](const auto& kv) {
        return kv.first == "PATH" && kv.second == "/usr/local/bin:/usr/bin:/bin";
    }));
    ASSERT_TRUE(std::any_of(env.binds.begin(), env.binds.end(),
                            [](const auto& b) { return b.host == "/etc" && b.sandbox == "/etc" && b.read_only; }));
}

TEST(EnvHostLoadsAsPlugin) {
    const char* dir = std::getenv("ZM_TEST_PLUGIN_DIR");
    ASSERT_TRUE(dir != nullptr);
    zeromesh::plugin::Registry reg;
    std::string err;
    ASSERT_TRUE(reg.LoadFile(fs::path(dir) / "libzm_env_host.so", &err));
    auto* env = reg.Env("host");
    ASSERT_TRUE(env != nullptr);
    ASSERT_TRUE(env->Prepare({}).value.has_value());
}

TEST(EnvHostResolvesAbsolutePathsOnlyInMountedDirectories) {
    ASSERT_TRUE(Resolve("/usr/bin/env").has_value());
    ASSERT_FALSE(Resolve("/home/nobody/tool").has_value());
    // A program on the requester's PATH but outside the sandbox's is not runnable there.
    const fs::path dir = fs::temp_directory_path() / "zm_env_host_unbound_path";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path tool = dir / "zm_only_here_tool";
    {
        std::ofstream out(tool);
        out << "#!/bin/sh\nexit 0\n";
    }
    fs::permissions(tool, fs::perms::owner_all, ec);
    ASSERT_FALSE(Resolve("zm_only_here_tool").has_value());
    ASSERT_FALSE(Resolve(tool.string()).has_value());
    fs::remove_all(dir, ec);
}
