#pragma once

#include "store/ignore.hpp"
#include "store/store.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zeromesh::store {

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
     * @brief Like BuildTree, skipping paths `rules` ignores. Adds the bytes of every
     * stored regular file to *total_bytes when it is non-null. Symlinks are stored as
     * symlinks and never followed. Throws std::runtime_error for file names containing
     * a newline or tab, which the manifest format cannot represent.
     */
    Digest BuildTree(const std::filesystem::path& root_dir, const IgnoreRules& rules, uint64_t* total_bytes);

    /**
     * @brief Returns a new root equal to `base_tree` with `item` placed at `at_path`
     * ('/'-separated, intermediate directories created). A tree item merges into an
     * existing directory there (the item wins on name clashes); a file item replaces
     * whatever had that name. An empty base_tree hash means an empty tree.
     */
    Digest Overlay(const Digest& base_tree, const std::string& at_path, const Digest& item, bool item_is_tree,
                   bool item_executable = false);

    /**
     * @brief Every digest reachable from the tree `root`: the root manifest, each sub-tree
     * manifest and each file blob, deduplicated, root first. An empty root hash gives an
     * empty list. Throws std::runtime_error if a manifest is missing or corrupt.
     */
    std::vector<Digest> CollectTree(const Digest& root);

    // A manifest entry name is safe when it is non-empty, not "." or "..", and has no '/' or NUL.
    static bool IsSafeName(std::string_view name);

    /**
     * @brief Reconstructs the filesystem structure from a CAS root directory digest.
     */
    bool MaterializeTree(const Digest& root_digest, const std::filesystem::path& destination);

private:
    Digest BuildDirectoryRecursive(const std::filesystem::path& current_path, const std::string& rel,
                                   const IgnoreRules& rules, uint64_t* total_bytes);
    DirectoryManifest LoadManifest(const Digest& tree);
    Digest MergeTrees(const Digest& base, const Digest& item);
    Digest OverlayRecursive(const Digest& base, const std::vector<std::string>& segs, size_t i, const Digest& item,
                            bool item_is_tree, bool item_executable);
    bool MaterializeDirectoryRecursive(const Digest& dir_digest, const std::filesystem::path& current_path);

    std::shared_ptr<CasStore> store_;
};

} // namespace zeromesh::store
