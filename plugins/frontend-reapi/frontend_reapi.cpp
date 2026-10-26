#include "frontend_reapi.hpp"

#include "plugin/client_api_handle.hpp"

#include <iostream>

namespace zeromesh::frontend_reapi {

Frontend::Frontend() = default;
Frontend::~Frontend() { Stop(); }

bool Frontend::Start(plugin::ClientApiHandle& api, const plugin::PluginConfig& cfg) {
    bool enabled = true;
    if (auto it = cfg.values.find("enabled"); it != cfg.values.end()) {
        enabled = (it->second == "1" || it->second == "true");
    } else if (const char* e = std::getenv("ZM_ENABLE_REAPI")) {
        enabled = (std::string_view(e) != "0" && std::string_view(e) != "false");
    }
    if (!enabled) {
        std::cerr << "[frontend-reapi] disabled by config\n";
        return true;
    }

    socket_ = api.socket_path();
    server_ = std::make_unique<ReapiServer>(api);
    if (!server_->Start()) {
        std::cerr << "[frontend-reapi] failed to listen on 127.0.0.1 (check port)\n";
        server_.reset();
        return false;
    }
    running_ = true;
    std::cerr << "[frontend-reapi] started against " << socket_ << " (CAS + Execution live)\n";
    return true;
}

void Frontend::Stop() {
    if (!running_) return;
    running_ = false;
    if (server_) { server_->Stop(); server_.reset(); }
    std::cerr << "[frontend-reapi] stopped\n";
}

} // namespace zeromesh::frontend_reapi