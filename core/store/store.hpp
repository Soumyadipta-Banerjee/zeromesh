#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

/** Disk and ingest limits for a donor node's CAS. */
struct CasLimits {
    // Fixed cap keeps donor disks predictable; 20 GiB fits typical dev/lab nodes without
    // requiring statvfs at startup (override via SetMaxBlobBytes or daemon config).
    uint64_t max_blob_bytes{20ULL * 1024 * 1024 * 1024};
    uint64_t peer_ingest_window_bytes{5ULL * 1024 * 1024 * 1024};
    std::chrono::seconds peer_ingest_window{3600};
};

/**
 * @brief Content-Addressed Storage (CAS) engine.
 * Stores raw blobs in `~/.zeromesh/cas/blobs/<sha256>` and action caches in `ac/<action_hash>`.
 */
class CasStore {
public:
    explicit CasStore(std::filesystem::path base_path = {}, CasLimits limits = {});
    ~CasStore() = default;

    // Non-copyable
    CasStore(const CasStore&) = delete;
    CasStore& operator=(const CasStore&) = delete;

    void SetMaxBlobBytes(uint64_t bytes);
    [[nodiscard]] uint64_t MaxBlobBytes() const;
    [[nodiscard]] uint64_t BlobBytes() const;
    [[nodiscard]] uint64_t ReservedIncomingBytes() const;

    struct GcStats {
        uint64_t bytes_freed{0};
        size_t blobs_removed{0};
    };
    /** Deletes least-recently-used blobs until total stored bytes is at most @p target_bytes. */
    GcStats CollectGarbage(uint64_t target_bytes, const std::unordered_set<std::string>& protect_hashes);

    /** True if @p bytes could be stored without exceeding the disk cap (ignores peer ingest window). */
    [[nodiscard]] bool CanAdmitIncoming(uint64_t bytes) const;

    /**
     * @brief Reserves capacity for an in-flight remote ingest (released on commit or cancel).
     * @return false when current + reserved + bytes would exceed MaxBlobBytes().
     */
    [[nodiscard]] bool ReserveIncoming(uint64_t bytes);
    [[nodiscard]] bool AdmitIncoming(uint64_t bytes) { return ReserveIncoming(bytes); }
    void ReleaseIncoming(uint64_t bytes);

    /**
     * @brief Accounts for a blob that was committed under @p hash (idempotent if already known).
     * Call after PutBlob/PutBlobFile or an atomic rename into blobs/.
     */
    void NoteBlobCommitted(const std::string& hash, uint64_t size_bytes);

    /** Rolling per-peer ingest cap plus disk cap. Does not reserve; pair with ReserveIncoming. */
    [[nodiscard]] bool AdmitPeerIngest(const std::string& peer_id, uint64_t bytes);
    void RecordPeerIngest(const std::string& peer_id, uint64_t bytes);

    /**
     * @brief Computes SHA-256 hash and byte size for in-memory data.
     */
    static Digest ComputeDigest(std::string_view data);

    // True for exactly 64 lowercase hex characters. Every lookup by hash rejects
    // anything else, so a hash received from a peer can never name a path.
    static bool IsValidHash(std::string_view hash);

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

    bool RemoveActionResult(const std::string& action_hash);

    /**
     * @brief Returns the base CAS directory.
     */
    [[nodiscard]] const std::filesystem::path& GetBasePath() const { return base_dir_; }

private:
    struct BlobMeta {
        uint64_t size{0};
        std::chrono::steady_clock::time_point last_used{};
    };

    struct PeerIngestWindow {
        uint64_t bytes{0};
        std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> events;
    };

    void ScanBlobsOnDisk();
    void TouchBlobLocked(const std::string& hash) const;
    void TouchBlobOnDisk(const std::filesystem::path& path) const;
    void PromoteLruLocked(const std::string& hash) const;
    void RegisterBlobLocked(const std::string& hash, uint64_t size,
                            std::chrono::steady_clock::time_point when) const;
    bool DeleteBlobLocked(const std::string& hash);
    void DropFromMemCacheLocked(const std::string& hash) const;
    void EvictBlobMetadataLocked(const std::string& hash) const;

    std::filesystem::path base_dir_;
    std::filesystem::path blobs_dir_;
    std::filesystem::path ac_dir_;

    CasLimits limits_;
    mutable std::mutex store_mutex_;
    mutable uint64_t blob_bytes_{0};
    uint64_t reserved_incoming_{0};
    mutable std::unordered_map<std::string, BlobMeta> blobs_;
    mutable std::list<std::string> lru_order_;
    mutable std::unordered_map<std::string, std::list<std::string>::iterator> lru_iters_;
    std::unordered_map<std::string, PeerIngestWindow> peer_ingest_;

    mutable std::mutex mem_cache_mutex_;
    mutable std::unordered_map<std::string, std::string> mem_cache_;
    mutable size_t cur_mem_bytes_{0};
    const size_t max_mem_bytes_{64 * 1024 * 1024}; // 64 MB hot in-memory cache
};

} // namespace zeromesh::store
