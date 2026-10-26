#include "frontend_reapi.hpp"

#include "plugin/client_api_handle.hpp"

#include <cstdlib>
#include <iostream>

namespace zeromesh::frontend_reapi {

Frontend::Frontend() = default;
Frontend::~Frontend() { Stop(); }

bool Frontend::Start(plugin::ClientApiHandle& api, const plugin::PluginConfig& cfg) {
    // Disabled by default; enable with PluginConfig enabled=true/1 or ZM_ENABLE_REAPI=1.
    bool enabled = false;
    if (auto it = cfg.values.find("enabled"); it != cfg.values.end()) {
        enabled = (it->second == "1" || it->second == "true");
    } else if (const char* e = std::getenv("ZM_ENABLE_REAPI")) {
        enabled = (std::string_view(e) == "1" || std::string_view(e) == "true");
    }
    if (!enabled) {
        std::cerr << "[frontend-reapi] disabled (set ZM_ENABLE_REAPI=1 to enable)\n";
        running_ = false;
        return true; // not an error — plugin stays loaded but idle
    }

    socket_ = api.socket_path();
    server_ = std::make_unique<ReapiServer>(api);
    if (!server_->Start()) {
        std::cerr << "[frontend-reapi] failed to listen on 127.0.0.1 (check port)\n";
        server_.reset();
        return false;
    }
    running_ = true;
    std::cerr << "[frontend-reapi] listening on 127.0.0.1"
              << (std::getenv("ZM_REAPI_PORT") ? std::string(":") + std::getenv("ZM_REAPI_PORT")
                                               : std::string(":8980"))
              << " via " << socket_ << "\n";
    return true;
}

void Frontend::Stop() {
    if (!running_) return;
    running_ = false;
    if (server_) { server_->Stop(); server_.reset(); }
    std::cerr << "[frontend-reapi] stopped\n";
}

} // namespace zeromesh::frontend_reapi