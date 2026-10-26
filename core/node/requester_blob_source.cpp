#include "node/requester_blob_source.hpp"

#include "node/convert.hpp"

namespace zeromesh::node {

RequesterBlobSource::RequesterBlobSource(std::shared_ptr<store::CasStore> cas, std::shared_ptr<NodeClient> requester)
    : cas_(std::move(cas)), requester_(std::move(requester)) {}

std::optional<std::string> RequesterBlobSource::FetchBlob(const std::string& hash) {
    if (!cas_ || !requester_) return std::nullopt;
    if (auto data = cas_->GetBlob(hash)) return data;
    std::lock_guard lock(mu_);
    if (auto data = cas_->GetBlob(hash)) return data;
    std::vector<v1::Digest> want;
    want.push_back(ToProto(store::Digest{.hash = hash, .size_bytes = 0}));
    std::string err;
    if (!requester_->Pull(*cas_, want, &err)) return std::nullopt;
    return cas_->GetBlob(hash);
}

} // namespace zeromesh::node
