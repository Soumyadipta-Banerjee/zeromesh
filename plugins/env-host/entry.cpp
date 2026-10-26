// Loadable-module entry point for env-host. Kept out of zm_core so the static
// library never defines zm_plugin_entry itself.
#include "plugins/env-host/env_host.hpp"

namespace {

zeromesh::plugin::IEnvProvider* MakeHost() { return zeromesh::envhost::Make().release(); }

const zeromesh::plugin::PluginEntry kEntry{
    .abi_major = zeromesh::plugin::kAbiMajor,
    .name = "host",
    .make_env = &MakeHost,
    .make_frontend = nullptr,
};

} // namespace

extern "C" const zeromesh::plugin::PluginEntry* zm_plugin_entry() { return &kEntry; }
