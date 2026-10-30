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
using zeromesh::plugin::EnvParams;

EnvParams Argv(std::string cmd) {
    EnvParams p;
    p.argv = {std::move(cmd)};
    return p;
}

} // namespace

TEST(EnvHostFindsSh) {
    auto env = zeromesh::envhost::Make();
    ASSERT_EQ(env->Name(), std::string_view("host"));
    ASSERT_TRUE(env->Check(Argv("sh")).ok);
    ASSERT_FALSE(env->Check({}).ok);
}

TEST(EnvHostMissingCommand) {
    auto a = zeromesh::envhost::Make()->Check(Argv("definitely-not-a-binary-xyz"));
    ASSERT_FALSE(a.ok);
    ASSERT_TRUE(a.reason.find("not found on this node") != std::string::npos);
    // Found on the host's PATH, but not on the search path this provider was given.
    ASSERT_FALSE(zeromesh::envhost::Make("/nonexistent")->Check(Argv("sh")).ok);
}

TEST(EnvHostRelativeScriptOk) {
    auto env = zeromesh::envhost::Make();
    ASSERT_TRUE(env->Check(Argv("./sim")).ok);
    ASSERT_TRUE(env->Check(Argv("bin/sim")).ok);
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
    ASSERT_TRUE(env->Check(Argv("sh")).ok);
}

TEST(EnvHostAbsolutePathInMountedDir) {
    auto env = zeromesh::envhost::Make();
    ASSERT_TRUE(env->Check({.params = {}, .argv = {"/usr/bin/env"}}).ok);
    ASSERT_FALSE(env->Check({.params = {}, .argv = {"/home/nobody/tool"}}).ok);
}

TEST(EnvHostRejectsCommandOnUnboundPath) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "zm_env_host_unbound_path";
    std::error_code ec;
    fs::create_directories(dir, ec);
    ASSERT_FALSE(ec);
    const fs::path tool = dir / "zm_only_here_tool";
    {
        std::ofstream out(tool);
        ASSERT_TRUE(out.good());
        out << "#!/bin/sh\nexit 0\n";
    }
    fs::permissions(tool, fs::perms::owner_all, ec);
    ASSERT_FALSE(ec);

    const std::string wide_path = dir.string() + ":" + zeromesh::envhost::DefaultPathEnv();
    ASSERT_TRUE(zeromesh::envhost::Make(wide_path)->Check(Argv("zm_only_here_tool")).ok);
    ASSERT_FALSE(zeromesh::envhost::Make()->Check(Argv("zm_only_here_tool")).ok);

    fs::remove(tool, ec);
    fs::remove(dir, ec);
}
