#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace zeromesh::store {

/**
 * @brief Content addressable digest combining SHA-256 hex string and payload byte size.
 */
struct Digest {
    std::string hash;
    int64_t size_bytes{0};

    bool operator==(const Digest& other) const {
        return hash == other.hash && size_bytes == other.size_bytes;
    }
};

/**
 * @brief Content-Addressed Storage (CAS) engine.
 * Stores raw blobs in `~/.zeromesh/cas/blobs/<sha256>` and action caches in `ac/<action_hash>`.
 */
class CasStore {
public:
    explicit CasStore(std::filesystem::path base_path = {});
    ~CasStore() = default;

    // Non-copyable
    CasStore(const CasStore&) = delete;
    CasStore& operator=(const CasStore&) = delete;

    /**
     * @brief Computes SHA-256 hash and byte size for in-memory data.
     */
    static Digest ComputeDigest(std::string_view data);

    /**
     * @brief Computes SHA-256 hash and byte size for a file on disk.
     */
    static Digest ComputeFileDigest(const std::filesystem::path& file_path);

    /**
     * @brief Stores an in-memory blob into CAS and returns its Digest.
     */
    Digest PutBlob(std::string_view data);

    /**
     * @brief Stores a file into CAS by linking or copying.
     */
    Digest PutBlobFile(const std::filesystem::path& src_file);

    /**
     * @brief Retrieves blob bytes by SHA-256 hash.
     */
    [[nodiscard]] std::optional<std::string> GetBlob(const std::string& hash) const;

    /**
     * @brief Checks if a blob exists in the CAS store.
     */
    [[nodiscard]] bool HasBlob(const std::string& hash) const;

    /**
     * @brief Returns the absolute filesystem path for a given blob hash.
     */
    [[nodiscard]] std::filesystem::path GetBlobPath(const std::string& hash) const;

    /**
     * @brief Saves serialized ActionResult into the local Action Cache (AC).
     */
    bool PutActionResult(const std::string& action_hash, std::string_view serialized_result);

    /**
     * @brief Fetches serialized ActionResult from the local Action Cache (AC).
     */
    [[nodiscard]] std::optional<std::string> GetActionResult(const std::string& action_hash) const;

    /**
     * @brief Returns the base CAS directory.
     */
    [[nodiscard]] const std::filesystem::path& GetBasePath() const { return base_dir_; }

private:
    std::filesystem::path base_dir_;
    std::filesystem::path blobs_dir_;
    std::filesystem::path ac_dir_;

    mutable std::mutex mem_cache_mutex_;
    mutable std::unordered_map<std::string, std::string> mem_cache_;
    size_t cur_mem_bytes_{0};
    const size_t max_mem_bytes_{64 * 1024 * 1024}; // 64 MB hot in-memory cache
};

} // namespace zeromesh::store
