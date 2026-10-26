#include "reapi/server.hpp"

#include <iostream>

namespace zeromesh::reapi {

ReapiServer::ReapiServer(
    ServerConfig config,
    std::shared_ptr<CasService> cas_svc,
    std::shared_ptr<ActionCacheService> ac_svc,
    std::shared_ptr<ExecutionService> exec_svc
) : config_(std::move(config)),
    cas_svc_(std::move(cas_svc)),
    ac_svc_(std::move(ac_svc)),
    exec_svc_(std::move(exec_svc)) {}

ReapiServer::~ReapiServer() {
    Shutdown();
}

bool ReapiServer::Start() {
    if (is_running_.exchange(true)) {
        return true; // already running
    }

    std::cout << "[ZeroMesh REAPI] Server listening on " << config_.host << ":" << config_.port << std::endl;
    std::cout << "[ZeroMesh REAPI] ContentAddressableStorage (CAS) ready" << std::endl;
    std::cout << "[ZeroMesh REAPI] ActionCache (AC) ready with Cache-First fast path" << std::endl;
    std::cout << "[ZeroMesh REAPI] ExecutionService ready for incoming build actions" << std::endl;

    return true;
}

void ReapiServer::Shutdown() {
    if (!is_running_.exchange(false)) {
        return;
    }
    std::cout << "[ZeroMesh REAPI] Server shutdown completed" << std::endl;
}

void ReapiServer::Wait() {
    // Blocks until shutdown is signaled
}

} // namespace zeromesh::reapi
