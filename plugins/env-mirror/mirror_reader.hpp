#pragma once

// The read path of the mirror, separated from FUSE so it can be counted, recorded and
// prefetched into without a mount. MirrorFuse's read() and directory loads go through it.

#include "plugins/env-mirror/chunk_source.hpp"
#include "plugins/env-mirror/env_profile.hpp"
#include "plugins/env-mirror/mirror_cache.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace zeromesh::mirror {

enum class FetchOrigin { kOnDemand, kPrefetch };

struct MirrorStats {
    uint64_t read_ops{0};          // file reads served (the FUSE round trips of the task)
    uint64_t on_demand_fetches{0}; // fetches from the requester a read or directory load had to wait for
    uint64_t cache_hits{0};        // reads and directory loads answered from the donor cache
    uint64_t prefetch_fetches{0};  // fetches the prefetcher made ahead of the task
    uint64_t prefetch_bytes{0};
};

class MirrorReader {
public:
    MirrorReader(std::shared_ptr<IChunkSource> source, std::shared_ptr<MirrorCache> cache);

    // Records first-touch order of file reads. Call before reads start.
    void SetRecorder(std::shared_ptr<AccessRecorder> recorder);

    // Copies up to `size` bytes of file `hash` (of `file_size` bytes) from `offset` into buf.
    // Returns the bytes copied (0 at or past the end), or -1 when the blob cannot be fetched.
    std::ptrdiff_t Read(const std::string& hash, uint64_t file_size, uint64_t offset, char* buf, size_t size);

    // A whole object (file or directory manifest): from the cache, else fetched and cached.
    std::optional<std::string> GetObject(const std::string& hash, FetchOrigin origin);

    // Makes sure `hash` is cached. True when it is cached afterwards; *fetched says whether this
    // call had to fetch it.
    bool Prefetch(const std::string& hash, bool* fetched = nullptr);

    [[nodiscard]] MirrorStats Stats() const;
    [[nodiscard]] const std::shared_ptr<MirrorCache>& cache() const { return cache_; }

private:
    std::shared_ptr<IChunkSource> source_;
    std::shared_ptr<MirrorCache> cache_;

    mutable std::mutex recorder_mu_;
    std::shared_ptr<AccessRecorder> recorder_;

    std::atomic<uint64_t> read_ops_{0};
    std::atomic<uint64_t> on_demand_{0};
    std::atomic<uint64_t> cache_hits_{0};
    std::atomic<uint64_t> prefetch_fetches_{0};
    std::atomic<uint64_t> prefetch_bytes_{0};
};

} // namespace zeromesh::mirror
