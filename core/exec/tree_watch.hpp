#pragma once

// Reports files that were finished under a directory tree: closed after writing, or renamed into
// place. Directories created later are watched as they appear. Used to commit each item of a
// combined batch (one process rendering many frames) the moment its output is complete.

#include <chrono>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeromesh::exec {

class TreeWatch {
public:
    // Watches `root` and every directory below it. ok() is false when inotify is unavailable.
    explicit TreeWatch(std::filesystem::path root);
    ~TreeWatch();
    TreeWatch(const TreeWatch&) = delete;
    TreeWatch& operator=(const TreeWatch&) = delete;

    [[nodiscard]] bool ok() const { return fd_ >= 0; }

    // Paths (relative to the root) finished since the last call; waits up to `timeout`.
    std::vector<std::string> Poll(std::chrono::milliseconds timeout);

private:
    void AddRecursive(const std::filesystem::path& dir);

    std::filesystem::path root_;
    int fd_{-1};
    std::unordered_map<int, std::filesystem::path> dirs_; // watch descriptor -> directory
};

} // namespace zeromesh::exec
