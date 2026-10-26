#pragma once

// frontend-reapi: REAPI v2 front-end on top of the core's ClientApi (M6).

#include "plugin/plugin.hpp"

#include "plugins/frontend-reapi/reapi_server.hpp"

#include <memory>
#include <string>

namespace zeromesh::frontend_reapi {

class Frontend : public plugin::IFrontend {
public:
    Frontend();
    ~Frontend() override;

    std::string_view Name() const override { return "frontend-reapi"; }
    bool Start(plugin::ClientApiHandle& api, const plugin::PluginConfig& cfg) override;
    void Stop() override;

private:
    std::string socket_;
    bool running_{false};
    std::unique_ptr<zeromesh::frontend_reapi::ReapiServer> server_;
};

} // namespace zeromesh::frontend_reapi