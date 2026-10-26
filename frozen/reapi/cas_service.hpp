#pragma once

#include "cas/store.hpp"

#include <memory>
#include <string>
#include <vector>

namespace zeromesh::reapi {

/**
 * @brief REAPI v2 ContentAddressableStorage service interface.
 * Implements FindMissingBlobs, BatchUpdateBlobs, BatchReadBlobs, and GetTree.
 */
class CasService {
public:
    explicit CasService(std::shared_ptr<cas::CasStore> store);
    virtual ~CasService() = default;

    /**
     * @brief Determines which requested digests are missing from the local CAS store.
     */
    [[nodiscard]] std::vector<cas::Digest> FindMissingBlobs(const std::vector<cas::Digest>& digests);

    /**
     * @brief Writes a batch of binary blobs into the CAS store.
     */
    bool BatchUpdateBlobs(const std::vector<std::pair<cas::Digest, std::string>>& blobs);

    /**
     * @brief Reads a batch of binary blobs by digest.
     */
    [[nodiscard]] std::vector<std::pair<cas::Digest, std::string>> BatchReadBlobs(const std::vector<cas::Digest>& digests);

private:
    std::shared_ptr<cas::CasStore> store_;
};

} // namespace zeromesh::reapi
