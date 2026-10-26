#include "reapi/cas_service.hpp"

namespace zeromesh::reapi {

CasService::CasService(std::shared_ptr<cas::CasStore> store)
    : store_(std::move(store)) {}

std::vector<cas::Digest> CasService::FindMissingBlobs(const std::vector<cas::Digest>& digests) {
    std::vector<cas::Digest> missing;
    for (const auto& digest : digests) {
        if (!store_->HasBlob(digest.hash)) {
            missing.push_back(digest);
        }
    }
    return missing;
}

bool CasService::BatchUpdateBlobs(const std::vector<std::pair<cas::Digest, std::string>>& blobs) {
    for (const auto& [digest, data] : blobs) {
        auto stored_digest = store_->PutBlob(data);
        if (stored_digest.hash != digest.hash) {
            return false; // hash verification failure
        }
    }
    return true;
}

std::vector<std::pair<cas::Digest, std::string>> CasService::BatchReadBlobs(const std::vector<cas::Digest>& digests) {
    std::vector<std::pair<cas::Digest, std::string>> result;
    for (const auto& digest : digests) {
        auto data = store_->GetBlob(digest.hash);
        if (data.has_value()) {
            result.emplace_back(digest, std::move(*data));
        }
    }
    return result;
}

} // namespace zeromesh::reapi
