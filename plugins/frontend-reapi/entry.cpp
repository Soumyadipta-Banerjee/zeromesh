#include "plugins/frontend-reapi/frontend_reapi.hpp"

#include "plugin/plugin.hpp"

extern "C" const zeromesh::plugin::PluginEntry* zm_plugin_entry() {
    static zeromesh::plugin::PluginEntry entry{
        zeromesh::plugin::kAbiMajor,
        "frontend-reapi",
        nullptr, // make_env
        []() -> zeromesh::plugin::IFrontend* {
            return new zeromesh::frontend_reapi::Frontend();
        },
    };
    return &entry;
}