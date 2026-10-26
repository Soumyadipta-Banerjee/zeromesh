#include "plugin/client_api_handle.hpp"

namespace zeromesh::plugin {

ClientApiHandle::ClientApiHandle(std::string socket_path,
                                std::shared_ptr<grpc::Channel> channel)
    : socket_path_(std::move(socket_path)),
      channel_(std::move(channel)),
      stub_(zeromesh::v1::ClientApi::NewStub(channel_)) {}

} // namespace zeromesh::plugin