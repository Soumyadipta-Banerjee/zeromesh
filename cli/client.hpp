#pragma once

#include "zeromesh/v1/client.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <filesystem>
#include <memory>
#include <string>

namespace zeromesh::cli {

inline constexpr const char* kNotRunning = "zmd is not running (start it with: zmd)";
inline constexpr int kExitNotRunning = 2;

// The daemon's socket, resolved exactly like zmd resolves it (ZM_RUNTIME_DIR, then
// $XDG_RUNTIME_DIR/zeromesh, then /tmp/zeromesh-<uid>).
std::filesystem::path DefaultSocketPath();

// A ClientApi stub over the daemon's unix socket.
class Client {
public:
    // nullptr when nothing answers on `socket` within a short deadline.
    static std::unique_ptr<Client> Connect(const std::filesystem::path& socket);

    v1::ClientApi::Stub& stub() { return *stub_; }

private:
    explicit Client(std::shared_ptr<grpc::Channel> channel);

    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<v1::ClientApi::Stub> stub_;
};

// Prints kNotRunning for UNAVAILABLE, else "zm: <what>: <message>", to stderr.
// Returns the exit code to use: kExitNotRunning or 1.
int ReportRpcError(const std::string& what, const grpc::Status& status);

} // namespace zeromesh::cli
