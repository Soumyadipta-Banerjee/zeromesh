#pragma once

#include "cas/store.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zeromesh::cas {

struct FileNode {
    std::string name;
    Digest digest;
    bool is_executable{false};
};

struct DirectoryNode {
    std::string name;
    Digest digest;
};

struct SymlinkNode {
    std::string name;
    std::string target;
};

struct DirectoryManifest {
    std::vector<FileNode> files;
    std::vector<DirectoryNode> directories;
    std::vector<SymlinkNode> symlinks;

    [[nodiscard]] std::string Serialize() const;
    static std::optional<DirectoryManifest> Deserialize(std::string_view data);
};

/**
 * @brief Constructs cryptographic Merkle trees from files and directories,
 * and materializes CAS directory trees onto the local disk.
 */
class MerkleBuilder {
public:
    explicit MerkleBuilder(std::shared_ptr<CasStore> store);
    ~MerkleBuilder() = default;

    /**
     * @brief Recursively hashes a directory on disk, storing all files in CAS,
     * and returns the root Directory digest.
     */
    Digest BuildTree(const std::filesystem::path& root_dir);

    /**
     * @brief Reconstructs the filesystem structure from a CAS root directory digest.
     */
    bool MaterializeTree(const Digest& root_digest, const std::filesystem::path& destination);

private:
    Digest BuildDirectoryRecursive(const std::filesystem::path& current_path);
    bool MaterializeDirectoryRecursive(const Digest& dir_digest, const std::filesystem::path& current_path);

    std::shared_ptr<CasStore> store_;
};

} // namespace zeromesh::cas
