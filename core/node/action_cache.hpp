#pragma once

#include "store/store.hpp"
#include "zeromesh/v1/common.pb.h"

#include <memory>
#include <optional>
#include <string>

namespace zeromesh::node {

// Persists v1::TaskResult (serialized proto) in the CasStore's action-result area, keyed by
// task_key. Only results whose content is still fully present in the CAS are returned.
class ActionCache {
public:
    explicit ActionCache(std::shared_ptr<store::CasStore> cas);

    // Returns the stored result only if it succeeded (exit 0, no error, no timeout) and every
    // non-symlink output digest plus the stdout/stderr digests are present in the CAS.
    [[nodiscard]] std::optional<v1::TaskResult> Get(const std::string& task_key) const;

    void Put(const std::string& task_key, const v1::TaskResult& result);

    // Removes on-disk cache entries that fail Get() (missing output blobs, etc.).
    void PruneStale();

private:
    std::shared_ptr<store::CasStore> cas_;
};

} // namespace zeromesh::node
