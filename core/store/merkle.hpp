#pragma once

#include "store/ignore.hpp"
#include "store/stat_cache.hpp"
#include "store/store.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

// How BuildTree and BuildFromPaths read the disk. Every pointer is optional.
struct TreeOptions {
    const IgnoreRules* ignore{nullptr};        // e.g. .zmignore: matching paths are skipped silently
    const IgnoreRules* deny{nullptr};          // secrets: matching paths are skipped and listed in `denied`
    StatCache* stat_cache{nullptr};            // reuse digests of files that did not change
    uint64_t* total_bytes{nullptr};            // += size of every stored regular file
    uint64_t* file_count{nullptr};             // += 1 per stored regular file
    std::vector<std::string>* denied{nullptr}; // relative paths `deny` kept out
};

// Materialize::Link hard-links files out of the store and is only for trees nothing will write
// to (a read-only overlay lower layer); a write through such a link would change the store.
// Materialize::Copy gives every file its own inode.
enum class Materialize { Copy, Link };

// One entry of a tree, found by path.
struct TreeEntry {
    enum class Kind { Absent, File, Directory, Symlink };
    Kind kind{Kind::Absent};
    Digest digest; // blob for files, manifest for directories
    bool executable{false};
    std::string target; // symlinks
};

/**
 * @brief Builds Merkle trees from the disk into the CAS, reads them back, and materializes them.
 * Manifests are the DirectoryManifest text format; files are CAS blobs.
 */
class MerkleBuilder {
public:
    explicit MerkleBuilder(std::shared_ptr<CasStore> store);
    ~MerkleBuilder() = default;

    /**
     * @brief Hashes the directory `root_dir` into the CAS and returns the root manifest digest
     * (an empty hash when `root_dir` is not a directory). Symlinks are stored as symlinks and
     * never followed. Throws std::runtime_error for names containing a newline or tab, which the
     * manifest format cannot represent.
     */
    Digest BuildTree(const std::filesystem::path& root_dir, const TreeOptions& opts = {});

    /**
     * @brief A tree holding `abs_paths` at their absolute positions (relative to "/"). Each path
     * may be a file or a directory (taken whole). A symlink met along a path is stored as a
     * symlink and its target is added too, so the tree resolves the same way the disk did.
     * Missing paths are skipped.
     */
    Digest BuildFromPaths(const std::vector<std::filesystem::path>& abs_paths, const TreeOptions& opts = {});

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

    // The entry at `rel` ('/'-separated; "" is the root itself). Symlinks inside the tree are
    // not followed. Throws std::runtime_error if a manifest on the way is missing or corrupt.
    TreeEntry Lookup(const Digest& root, std::string_view rel);

    // Sorted names in the directory at `rel`; nullopt when there is no directory there.
    std::optional<std::vector<std::string>> ListNames(const Digest& root, std::string_view rel);

    // Up to `limit` paths whose entry differs between the trees (added, removed or changed),
    // depth first in name order.
    std::vector<std::string> Diff(const Digest& a, const Digest& b, size_t limit);

    // A manifest entry name is safe when it is non-empty, not "." or "..", and has no '/' or NUL.
    static bool IsSafeName(std::string_view name);

    /**
     * @brief Writes the tree `root_digest` to `destination` (created). False when a blob or
     * manifest is missing, or a manifest names an unsafe entry.
     */
    bool MaterializeTree(const Digest& root_digest, const std::filesystem::path& destination,
                         Materialize mode = Materialize::Copy);

private:
    struct PathNode;

    Digest BuildDirectoryRecursive(const std::filesystem::path& current_path, const std::string& rel,
                                   const TreeOptions& opts);
    Digest HashFile(const std::filesystem::path& path, const TreeOptions& opts);
    bool Skipped(const std::string& rel, bool is_dir, const TreeOptions& opts);
    void InsertPath(PathNode& root, const std::filesystem::path& abs, const TreeOptions& opts, int depth);
    void InsertDirectory(PathNode& node, const std::filesystem::path& dir, const std::string& rel,
                         const TreeOptions& opts);
    Digest StoreNode(const PathNode& node);
    DirectoryManifest LoadManifest(const Digest& tree);
    Digest MergeTrees(const Digest& base, const Digest& item);
    Digest OverlayRecursive(const Digest& base, const std::vector<std::string>& segs, size_t i, const Digest& item,
                            bool item_is_tree, bool item_executable);
    bool MaterializeDirectoryRecursive(const Digest& dir_digest, const std::filesystem::path& current_path,
                                       Materialize mode);
    void DiffRecursive(const Digest& a, const Digest& b, const std::string& prefix, size_t limit,
                       std::vector<std::string>& out);

    std::shared_ptr<CasStore> store_;
};

} // namespace zeromesh::store
