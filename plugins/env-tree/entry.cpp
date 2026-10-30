// Loadable-module entry point for env-tree. Kept out of zm_core so the static
// library never defines zm_plugin_entry itself.
#include "plugins/env-tree/env_tree.hpp"

namespace {

zeromesh::plugin::IEnvProvider* MakeTree() {
    return zeromesh::tree::MakeTreeEnv().release();
}

const zeromesh::plugin::PluginEntry kEntry{
    .abi_major = zeromesh::plugin::kAbiMajor,
    .name = "tree",
    .make_env = &MakeTree,
    .make_frontend = nullptr,
};

} // namespace

extern "C" const zeromesh::plugin::PluginEntry* zm_plugin_entry() {
    return &kEntry;
}
