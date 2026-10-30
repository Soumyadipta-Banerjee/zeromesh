// Loadable-module entry point for env-mirror. Kept out of zm_core so the static
// library never defines zm_plugin_entry itself.
#include "plugins/env-mirror/env_mirror.hpp"

namespace {

zeromesh::plugin::IEnvProvider* MakeMirror() {
    return zeromesh::mirror::MakeMirrorEnv().release();
}

const zeromesh::plugin::PluginEntry kEntry{
    .abi_major = zeromesh::plugin::kAbiMajor,
    .name = "mirror",
    .make_env = &MakeMirror,
    .make_frontend = nullptr,
};

} // namespace

extern "C" const zeromesh::plugin::PluginEntry* zm_plugin_entry() {
    return &kEntry;
}
