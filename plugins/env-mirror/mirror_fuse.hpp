#pragma once

#include "plugins/env-mirror/chunk_source.hpp"
#include "plugins/env-mirror/mirror_cache.hpp"
#include "store/ignore.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::mirror {

/**
 * @brief In-memory directory entry in the virtual tree.
 */
struct VirtualNode {
    std::string name;
    bool is_directory{false};
    bool is_symlink{false};
    bool is_executable{false};
    uint64_t size_bytes{0};
    std::string digest_hash; // SHA-256 hex
    std::string symlink_target;
    std::unordered_map<std::string, std::shared_ptr<VirtualNode>> children;
};

/**
 * @brief Read-only FUSE filesystem providing access to the requester's files.
 *
 * All modifying operations return EROFS. Missing file data is fetched lazily
 * via IChunkSource and cached in MirrorCache.
 */
class MirrorFuse {
public:
    MirrorFuse(std::filesystem::path mount_point,
               std::shared_ptr<IChunkSource> chunk_source,
               std::shared_ptr<MirrorCache> cache);
    ~MirrorFuse();

    MirrorFuse(const MirrorFuse&) = delete;
    MirrorFuse& operator=(const MirrorFuse&) = delete;

    /// Checks if FUSE kernel support and permissions are available.
    static bool IsFuseAvailable();

    /// Checks if a given path is currently mounted according to /proc/self/mounts.
    static bool IsPathMounted(const std::filesystem::path& path);

    /// Lazy-unmount via fusermount3 without invoking a shell.
    static bool FusermountUnmountLazy(const std::filesystem::path& mount_point);

    /// Mounts the filesystem using a root Merkle manifest digest.
    bool Mount(const std::string& root_manifest_digest);

    /// Populates the virtual tree from a root VirtualNode directly.
    bool MountWithRoot(std::shared_ptr<VirtualNode> root_node);

    /// Unmounts cleanly, ensuring no stale mounts remain in /proc/self/mounts.
    void Unmount();

    /// Returns true if currently mounted and running.
    [[nodiscard]] bool IsMounted() const;

    /// Returns the mount point path.
    [[nodiscard]] const std::filesystem::path& MountPoint() const { return mount_point_; }

    /// Finds a node by its virtual path (e.g. "/project/main.c").
    [[nodiscard]] std::shared_ptr<VirtualNode> Lookup(std::string_view path);

    struct Impl;

private:
    std::filesystem::path mount_point_;
    std::shared_ptr<IChunkSource> chunk_source_;
    std::shared_ptr<MirrorCache> cache_;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeromesh::mirror
