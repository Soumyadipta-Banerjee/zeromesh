#pragma once

#include "store/store.hpp"
#include "zeromesh/v1/common.pb.h"

#include <map>
#include <memory>
#include <mutex>
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

    // Stores a finished result and drops the partial entry of the same key (see PutUnit).
    void Put(const std::string& task_key, const v1::TaskResult& result);

    // Partial commit: units (a frame, a sweep case) a task finished before it was interrupted.
    // They are kept per task key, apart from the result, until the task succeeds, so a retry
    // or re-run of the same task can start from them. Safe to call from several threads.
    void PutUnit(const std::string& task_key, const std::string& unit_id, const v1::Digest& digest);
    // The units saved for `task_key` whose id is valid and whose blob is still in the CAS.
    [[nodiscard]] std::map<std::string, v1::Digest> GetUnits(const std::string& task_key) const;

    // Removes on-disk cache entries that fail Get() (missing output blobs, etc.). Partial
    // entries lose their units whose blob is gone and are removed once none is left.
    void PruneStale();

private:
    // The partial entry for a key lives under its own address; it is a TaskResult marked with
    // kPartialMarker whose outputs are (unit id, digest) pairs, so the result lookup never sees it.
    [[nodiscard]] static std::string UnitsKey(const std::string& task_key);
    [[nodiscard]] std::optional<v1::TaskResult> ReadUnitsEntry(const std::string& units_key) const;
    void WriteUnitsEntry(const std::string& units_key, const v1::TaskResult& entry);

    std::shared_ptr<store::CasStore> cas_;
    mutable std::mutex units_mu_; // serializes read-modify-write of partial entries
};

} // namespace zeromesh::node
