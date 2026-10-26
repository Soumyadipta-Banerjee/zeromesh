#include "cli/client.hpp"

#include "node/daemon.hpp"

#include <chrono>
#include <iostream>

namespace zeromesh::cli {

std::filesystem::path DefaultSocketPath() { return node::DaemonConfig::FromEnv().socket_path(); }

Client::Client(std::shared_ptr<grpc::Channel> channel)
    : channel_(std::move(channel)), stub_(v1::ClientApi::NewStub(channel_)) {}

std::unique_ptr<Client> Client::Connect(const std::filesystem::path& socket) {
    // A plain connect() fails at once for a missing socket or a dead daemon, where gRPC
    // would keep retrying until its deadline.
    if (!node::UnixSocketAlive(socket)) return nullptr;
    auto channel = grpc::CreateChannel("unix:" + socket.string(), grpc::InsecureChannelCredentials());
    if (!channel->WaitForConnected(std::chrono::system_clock::now() + std::chrono::seconds(3))) return nullptr;
    return std::unique_ptr<Client>(new Client(std::move(channel)));
}

int ReportRpcError(const std::string& what, const grpc::Status& status) {
    if (status.error_code() == grpc::StatusCode::UNAVAILABLE) {
        std::cerr << kNotRunning << "\n";
        return kExitNotRunning;
    }
    std::cerr << "zm: " << what << ": " << status.error_message() << "\n";
    return 1;
}

} // namespace zeromesh::cli
