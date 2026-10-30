// Test-only environment provider, built as a loadable plugin module.
// Compiled twice: normally, and with -DFAKE_ABI=99 to exercise ABI rejection.
#include "plugin/plugin.hpp"

#ifndef FAKE_ABI
#define FAKE_ABI zeromesh::plugin::kAbiMajor
#endif

namespace {

class FakeEnv final : public zeromesh::plugin::IEnvProvider {
public:
    std::string_view Name() const override { return "fake"; }
    zeromesh::plugin::Availability Check(const zeromesh::plugin::EnvParams&) override {
        return {.ok = true, .reason = {}};
    }
    zeromesh::plugin::Result<zeromesh::plugin::SandboxEnv> Prepare(const zeromesh::plugin::TaskContext&) override {
        return {.value = zeromesh::plugin::SandboxEnv{}, .error = {}};
    }
    void Release(const zeromesh::plugin::TaskContext&) override {}
};

zeromesh::plugin::IEnvProvider* MakeFake() {
    return new FakeEnv();
}

const zeromesh::plugin::PluginEntry kEntry{
    .abi_major = FAKE_ABI,
    .name = "fake",
    .make_env = &MakeFake,
    .make_frontend = nullptr,
};

} // namespace

extern "C" const zeromesh::plugin::PluginEntry* zm_plugin_entry() {
    return &kEntry;
}
