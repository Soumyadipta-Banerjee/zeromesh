#pragma once

#include "plugin/client_api_handle.hpp"

#include "remote_execution.pb.h"
#include "remote_execution.grpc.pb.h"
#include "bytestream.pb.h"
#include "bytestream.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace zeromesh::frontend_reapi {

namespace reapi = build::bazel::remote::execution::v2;

class CasServiceImpl final : public reapi::ContentAddressableStorage::Service {
public:
    explicit CasServiceImpl(plugin::ClientApiHandle& api) : api_(api) {}
    grpc::Status FindMissingBlobs(grpc::ServerContext* ctx,
        const reapi::FindMissingBlobsRequest* req,
        reapi::FindMissingBlobsResponse* resp) override;
    grpc::Status BatchUpdateBlobs(grpc::ServerContext* ctx,
        const reapi::BatchUpdateBlobsRequest* req,
        reapi::BatchUpdateBlobsResponse* resp) override;
    grpc::Status BatchReadBlobs(grpc::ServerContext* ctx,
        const reapi::BatchReadBlobsRequest* req,
        reapi::BatchReadBlobsResponse* resp) override;
    grpc::Status GetTree(grpc::ServerContext* ctx,
        const reapi::GetTreeRequest* req,
        reapi::GetTreeResponse* resp) override;
private:
    plugin::ClientApiHandle& api_;
};

class ActionCacheServiceImpl final : public reapi::ActionCache::Service {
public:
    grpc::Status GetActionResult(grpc::ServerContext* ctx,
        const reapi::GetActionResultRequest* req,
        reapi::ActionResult* resp) override;
    grpc::Status UpdateActionResult(grpc::ServerContext* ctx,
        const reapi::UpdateActionResultRequest* req,
        reapi::ActionResult* resp) override;
private:
    std::mutex mu_;
    std::map<std::string, reapi::ActionResult> cache_;
};

class ExecutionServiceImpl final : public reapi::Execution::Service {
public:
    ExecutionServiceImpl(plugin::ClientApiHandle& api, std::shared_ptr<ActionCacheServiceImpl> ac)
        : api_(api), ac_(std::move(ac)) {}
    grpc::Status Execute(grpc::ServerContext* ctx,
        const reapi::ExecuteRequest* req,
        grpc::ServerWriter<reapi::Operation>* writer) override;
    grpc::Status WaitExecution(grpc::ServerContext* ctx,
        const reapi::WaitExecutionRequest* req,
        grpc::ServerWriter<reapi::Operation>* writer) override;
private:
    plugin::ClientApiHandle& api_;
    std::shared_ptr<ActionCacheServiceImpl> ac_;
    std::string FetchBlob(const reapi::Digest& d);
    void MaterializeDir(const reapi::Digest& dirDigest, const std::filesystem::path& base);
};

class ByteStreamServiceImpl final : public google::bytestream::ByteStream::Service {
public:
    explicit ByteStreamServiceImpl(plugin::ClientApiHandle& api) : api_(api) {}
    grpc::Status Read(grpc::ServerContext* ctx,
        const google::bytestream::ReadRequest* req,
        grpc::ServerWriter<google::bytestream::ReadResponse>* writer) override;
    grpc::Status Write(grpc::ServerContext* ctx,
        grpc::ServerReader<google::bytestream::WriteRequest>* reader,
        google::bytestream::WriteResponse* resp) override;
    grpc::Status QueryWriteStatus(grpc::ServerContext* ctx,
        const google::bytestream::QueryWriteStatusRequest* req,
        google::bytestream::QueryWriteStatusResponse* resp) override;
private:
    plugin::ClientApiHandle& api_;
};

class CapabilitiesServiceImpl final : public reapi::Capabilities::Service {
public:
    grpc::Status GetCapabilities(grpc::ServerContext* ctx,
        const reapi::GetCapabilitiesRequest* req,
        reapi::ServerCapabilities* resp) override;
};

class ReapiServer {
public:
    explicit ReapiServer(plugin::ClientApiHandle& api);
    ~ReapiServer();

    bool Start();
    void Stop();

private:
    plugin::ClientApiHandle& api_;
    std::shared_ptr<CasServiceImpl> cas_;
    std::shared_ptr<ActionCacheServiceImpl> ac_;
    std::shared_ptr<ExecutionServiceImpl> exec_;
    std::shared_ptr<ByteStreamServiceImpl> bytestream_;
    std::shared_ptr<CapabilitiesServiceImpl> caps_;
    std::unique_ptr<grpc::Server> server_;
};

} // namespace zeromesh::frontend_reapi