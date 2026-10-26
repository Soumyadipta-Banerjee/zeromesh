#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::vfs {

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
};

/**
 * @brief Inode metadata representing a distributed file in zm-fs.
 */
struct FileInode {
    uint64_t inode_number{0};
    uint32_t mode{0};
    uint64_t size_bytes{0};
    uint64_t mtime_sec{0};
    std::vector<std::string> chunk_hashes; // Ordered BLAKE3 / SHA-256 digests
};

/**
 * @brief ZeroMesh Distributed Secure FUSE Filesystem (`zm-fs`).
 * Mounts a user-space virtual filesystem using Linux fuse3.
 * Transparently lazy-faults missing blocks via the Kademlia DHT and peer swarms.
 */
class FuseFileSystem {
public:
    explicit FuseFileSystem(FuseConfig config);
    ~FuseFileSystem();

    // Non-copyable
    FuseFileSystem(const FuseFileSystem&) = delete;
    FuseFileSystem& operator=(const FuseFileSystem&) = delete;

    /**
     * @brief Mounts the filesystem at the configured mount point in the background.
     */
    bool Mount();

    /**
     * @brief Unmounts the filesystem cleanly.
     */
    void Unmount();

    /**
     * @brief Checks if the filesystem is currently mounted and active.
     */
    [[nodiscard]] bool IsMounted() const;

    /**
     * @brief Manually stages and hashes a file into the distributed VFS.
     */
    std::string PublishFile(const std::filesystem::path& local_src, std::string_view virtual_dest);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeromesh::vfs
