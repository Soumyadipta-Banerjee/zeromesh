#pragma once

// Concrete ClientApiHandle: a small wrapper around the daemon's own ClientApi
// unix socket that front-end plugins (e.g. frontend-reapi) use to drive the core.
// Defined here (not in plugin.hpp) so plugin.hpp stays free of generated gRPC
// headers; plugins that need it include this header explicitly.

#include "plugin/plugin.hpp"

#include "zeromesh/v1/client.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

namespace zeromesh::plugin {

class ClientApiHandle {
public:
    ClientApiHandle(std::string socket_path,
                    std::shared_ptr<grpc::Channel> channel);

    const std::string& socket_path() const { return socket_path_; }
    grpc::Channel& channel() const { return *channel_; }
    zeromesh::v1::ClientApi::Stub& stub() { return *stub_; }

private:
    std::string socket_path_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<zeromesh::v1::ClientApi::Stub> stub_;
};

} // namespace zeromesh::plugin