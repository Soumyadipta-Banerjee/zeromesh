#pragma once

#include "node/node_client.hpp"
#include "plugin/plugin.hpp"
#include "store/store.hpp"

#include <memory>
#include <mutex>
#include <string>

namespace zeromesh::node {

// Reads blobs from the local CAS, pulling from a remote requester on miss.
class RequesterBlobSource final : public plugin::IBlobSource {
public:
    RequesterBlobSource(std::shared_ptr<store::CasStore> cas, std::shared_ptr<NodeClient> requester);

    std::optional<std::string> FetchBlob(const std::string& hash) override;

private:
    std::shared_ptr<store::CasStore> cas_;
    std::shared_ptr<NodeClient> requester_;
    std::mutex mu_;
};

// Local CAS only (requester runs on this node).
class LocalCasBlobSource final : public plugin::IBlobSource {
public:
    explicit LocalCasBlobSource(std::shared_ptr<store::CasStore> cas) : cas_(std::move(cas)) {}

    std::optional<std::string> FetchBlob(const std::string& hash) override {
        if (!cas_) return std::nullopt;
        return cas_->GetBlob(hash);
    }

private:
    std::shared_ptr<store::CasStore> cas_;
};

} // namespace zeromesh::node
