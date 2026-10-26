#pragma once

#include "reapi/action_cache.hpp"
#include "reapi/cas_service.hpp"
#include "reapi/execution_service.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace zeromesh::reapi {

struct ServerConfig {
    std::string host{"127.0.0.1"};
    int port{8980};
    size_t max_receive_message_size{128 * 1024 * 1024}; // 128 MB
    size_t max_send_message_size{128 * 1024 * 1024};
};

/**
 * @brief ZeroMesh REAPI gRPC Server coordinator.
 * Listens on 127.0.0.1:8980 for build tool connections (recc, Bazel, Buck2).
 */
class ReapiServer {
public:
    ReapiServer(
        ServerConfig config,
        std::shared_ptr<CasService> cas_svc,
        std::shared_ptr<ActionCacheService> ac_svc,
        std::shared_ptr<ExecutionService> exec_svc
    );
    ~ReapiServer();

    /**
     * @brief Binds port and starts serving gRPC requests.
     */
    bool Start();

    /**
     * @brief Initiates graceful server shutdown.
     */
    void Shutdown();

    /**
     * @brief Blocks until server shutdown completes.
     */
    void Wait();

    [[nodiscard]] bool IsRunning() const { return is_running_.load(); }
    [[nodiscard]] int GetPort() const { return config_.port; }

private:
    ServerConfig config_;
    std::shared_ptr<CasService> cas_svc_;
    std::shared_ptr<ActionCacheService> ac_svc_;
    std::shared_ptr<ExecutionService> exec_svc_;
    std::atomic<bool> is_running_{false};
};

} // namespace zeromesh::reapi
