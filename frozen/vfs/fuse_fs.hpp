#pragma once

#include "cas/store.hpp"
#include "dht/kademlia.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zeromesh::vfs {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Configuration parameters for the Distributed Secure FUSE Filesystem.
 */
struct FuseConfig {
    std::filesystem::path mount_point;
    std::filesystem::path cache_dir{"~/.zeromesh/vfs_cache"};
    size_t chunk_size_bytes{512 * 1024}; // 512 KB chunks
    bool read_only{false};
    bool enable_chacha20_encryption{true};
    std::string volume_key_hex;

    /// Maximum local chunk cache size in bytes (LRU eviction beyond this).
    size_t max_cache_bytes{512 * 1024 * 1024}; // 512 MB

    /// TCP port for serving chunk data to peers requesting our chunks.
    uint16_t chunk_serve_port{8984};

    /// Timeout for chunk fetch from remote peers.
    std::chrono::milliseconds fetch_timeout{5000};

    /// Number of parallel chunk prefetch connections.
    size_t prefetch_window{4};
};

// ─────────────────────────────────────────────────────────────────────────────
// Inode Metadata
// ─────────────────────────────────────────────────────────────────────────────

/// Inode types mirroring POSIX.
enum class InodeType : uint8_t {
    RegularFile = 0,
    Directory   = 1,
    Symlink     = 2,
};

/**
 * @brief Inode metadata representing a distributed file in zm-fs.
 *
 * Files are split into fixed-size chunks (default 512 KB). Each chunk is
 * content-addressed by its SHA-256 hash. The ordered list of chunk hashes
 * is stored in `chunk_hashes`, allowing lazy block-level faulting via the
 * Kademlia DHT provider network.
 */
struct FileInode {
    uint64_t inode_number{0};
    InodeType type{InodeType::RegularFile};
    uint32_t mode{0644};          ///< POSIX permission bits.
    uint32_t uid{0};
    uint32_t gid{0};
    uint64_t size_bytes{0};
    uint64_t mtime_sec{0};
    uint64_t ctime_sec{0};
    uint64_t atime_sec{0};
    uint32_t nlink{1};

    /// Ordered content-addressed chunk digests (SHA-256 hex strings).
    /// Only valid for RegularFile inodes.
    std::vector<std::string> chunk_hashes;

    /// Symlink target path (only valid for Symlink inodes).
    std::string symlink_target;
};

/**
 * @brief A single directory entry linking a name to an inode number.
 */
struct DirEntry {
    std::string name;
    uint64_t inode_number{0};
    InodeType type{InodeType::RegularFile};
};

// ─────────────────────────────────────────────────────────────────────────────
// Chunk Cache Statistics
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Runtime statistics for the VFS layer.
 */
struct VfsStats {
    std::atomic<uint64_t> chunks_served_local{0};
    std::atomic<uint64_t> chunks_fetched_remote{0};
    std::atomic<uint64_t> chunk_fetch_failures{0};
    std::atomic<uint64_t> bytes_read{0};
    std::atomic<uint64_t> bytes_written{0};
    std::atomic<uint64_t> files_published{0};
    std::atomic<uint64_t> cache_evictions{0};
};

// ─────────────────────────────────────────────────────────────────────────────
// FuseFileSystem — DHT-Backed Distributed FUSE VFS
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief ZeroMesh Distributed Secure FUSE Filesystem (`zm-fs`).
 *
 * Mounts a user-space virtual filesystem using Linux fuse3.
 * Transparently lazy-faults missing blocks via the Kademlia DHT and peer swarms.
 *
 * Architecture:
 * ┌─────────────┐    ┌────────────────┐    ┌──────────────┐
 * │   FUSE ops  │───>│  Inode Table   │───>│ Chunk Cache  │
 * │ (read/write │    │ (virtual dirs, │    │ (LRU disk +  │
 * │  readdir,   │    │  file inodes)  │    │  mem cache)  │
 * │  getattr)   │    └────────┬───────┘    └──────┬───────┘
 * └─────────────┘             │                   │ miss
 *                             │                   ▼
 *                    ┌────────┴───────┐    ┌──────────────┐
 *                    │  CAS Store     │    │ ChunkFetcher │
 *                    │ (local blobs)  │    │ (DHT lookup  │
 *                    └────────────────┘    │  + TCP pull) │
 *                                         └──────┬───────┘
 *                                                │
 *                                         ┌──────┴───────┐
 *                                         │ KademliaDht  │
 *                                         │ (providers)  │
 *                                         └──────────────┘
 */
class FuseFileSystem {
public:
    /**
     * @brief Constructs a VFS backed by the given DHT and CAS store.
     *
     * @param config       VFS configuration (mount point, cache dir, etc.)
     * @param dht          Shared Kademlia DHT engine for chunk provider discovery.
     * @param cas_store    Shared CAS store for local blob storage.
     */
    FuseFileSystem(FuseConfig config,
                   std::shared_ptr<dht::KademliaDht> dht,
                   std::shared_ptr<cas::CasStore> cas_store);
    ~FuseFileSystem();

    // Non-copyable
    FuseFileSystem(const FuseFileSystem&) = delete;
    FuseFileSystem& operator=(const FuseFileSystem&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────

    /**
     * @brief Mounts the filesystem at the configured mount point in the background.
     * Starts the FUSE event loop, the chunk server, and the cache janitor thread.
     */
    bool Mount();

    /**
     * @brief Unmounts the filesystem cleanly.
     * Stops the FUSE event loop, drains in-flight chunk fetches,
     * and flushes dirty metadata.
     */
    void Unmount();

    /**
     * @brief Checks if the filesystem is currently mounted and active.
     */
    [[nodiscard]] bool IsMounted() const;

    // ── File Publishing ─────────────────────────────────────────────────

    /**
     * @brief Stages a local file into the distributed VFS.
     *
     * 1. Splits the file into chunk_size_bytes chunks.
     * 2. Hashes each chunk (SHA-256) and stores it in CAS.
     * 3. Announces each chunk hash via the DHT.
     * 4. Creates a virtual inode entry at `virtual_dest`.
     *
     * @param local_src     Path to the local source file.
     * @param virtual_dest  Virtual path within the VFS (e.g. "/src/main.cpp").
     * @return Root hash of the file's chunk manifest, or empty string on failure.
     */
    std::string PublishFile(const std::filesystem::path& local_src,
                            std::string_view virtual_dest);

    /**
     * @brief Recursively publishes an entire directory tree into the VFS.
     *
     * @param local_dir     Local directory to publish.
     * @param virtual_dest  Virtual directory path in the VFS.
     * @return Number of files successfully published.
     */
    size_t PublishDirectory(const std::filesystem::path& local_dir,
                            std::string_view virtual_dest);

    // ── Inode Operations (for external queries / CLI) ───────────────────

    /**
     * @brief Looks up an inode by its virtual path.
     */
    [[nodiscard]] std::optional<FileInode> Lookup(std::string_view path) const;

    /**
     * @brief Lists entries in a virtual directory.
     */
    [[nodiscard]] std::vector<DirEntry> ReadDir(std::string_view path) const;

    // ── Statistics ──────────────────────────────────────────────────────

    /**
     * @brief Returns a reference to the runtime statistics counters.
     */
    [[nodiscard]] const VfsStats& GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeromesh::vfs
