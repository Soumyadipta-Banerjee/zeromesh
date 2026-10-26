#include "tests/unit/test_framework.hpp"
#include "plugin/loader.hpp"

#include <cstdlib>
#include <filesystem>

namespace {

std::filesystem::path PluginDir() {
    const char* dir = std::getenv("ZM_TEST_PLUGIN_DIR");
    if (dir == nullptr) throw std::runtime_error("ZM_TEST_PLUGIN_DIR not set");
    return dir;
}

class StaticEnv final : public zeromesh::plugin::IEnvProvider {
public:
    std::string_view Name() const override { return "static"; }
    zeromesh::plugin::Availability Check(const zeromesh::plugin::EnvParams&) override { return {.ok = true, .reason = {}}; }
    zeromesh::plugin::Result<zeromesh::plugin::SandboxEnv> Prepare(const zeromesh::plugin::TaskContext&) override {
        return {.value = zeromesh::plugin::SandboxEnv{}, .error = {}};
    }
    void Release(const zeromesh::plugin::TaskContext&) override {}
};

} // namespace

TEST(PluginLoadsSharedModule) {
    zeromesh::plugin::Registry reg;
    std::string err;
    ASSERT_TRUE(reg.LoadFile(PluginDir() / "libzm_fake_env.so", &err));
    auto* env = reg.Env("fake");
    ASSERT_TRUE(env != nullptr);
    ASSERT_TRUE(env->Check({}).ok);
}

TEST(PluginRejectsAbiMismatch) {
    zeromesh::plugin::Registry reg;
    std::string err;
    ASSERT_FALSE(reg.LoadFile(PluginDir() / "libzm_fake_env_badabi.so", &err));
    ASSERT_TRUE(err.find("abi") != std::string::npos);
    ASSERT_TRUE(reg.Env("fake") == nullptr);
}

TEST(PluginStaticRegistration) {
    zeromesh::plugin::Registry reg;
    reg.RegisterEnv(std::make_unique<StaticEnv>());
    ASSERT_TRUE(reg.Env("static") != nullptr);
    ASSERT_EQ(reg.EnvNames().size(), static_cast<size_t>(1));
}

TEST(PluginMissingFileReportsError) {
    zeromesh::plugin::Registry reg;
    std::string err;
    ASSERT_FALSE(reg.LoadFile("/nonexistent/libnope.so", &err));
    ASSERT_FALSE(err.empty());
}
