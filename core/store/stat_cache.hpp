#pragma once

// Remembers the digest of every file a snapshot hashed, keyed by its absolute path and the
// (device, inode, size, mtime, ctime) it had then. A later snapshot of an unchanged file reuses
// the digest instead of reading the file again, so re-snapshotting a large workspace costs a
// stat per file. A file modified within a second of being hashed is never trusted (its mtime
// may not have moved), the same rule git uses for its index.

#include "store/store.hpp"

#include <sys/stat.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace zeromesh::store {

class StatCache {
public:
    // Loads `file` if it exists; an unreadable or corrupt file starts an empty cache.
    explicit StatCache(std::filesystem::path file);

    // The digest recorded for `path` if `st` still matches what was recorded.
    [[nodiscard]] std::optional<Digest> Lookup(const std::string& path, const struct stat& st);
    void Store(const std::string& path, const struct stat& st, const Digest& digest);

    // Writes the entries looked up or stored since the cache was loaded; the others belong to
    // files that are gone, and are dropped.
    void Save();

    [[nodiscard]] size_t hits() const;
    [[nodiscard]] size_t misses() const;

private:
    struct Entry {
        uint64_t dev{0};
        uint64_t ino{0};
        int64_t size{0};
        int64_t mtime_ns{0};
        int64_t ctime_ns{0};
        int64_t hashed_at_ns{0};
        Digest digest;
        bool used{false};
    };

    static bool Matches(const Entry& e, const struct stat& st);

    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
    size_t hits_{0};
    size_t misses_{0};
};

} // namespace zeromesh::store
