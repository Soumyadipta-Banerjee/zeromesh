#pragma once

// One read-only copy of each input tree a donor is running, shared by every batch that reads it.
// Files are hard links into the CAS, which is safe only because nothing ever writes here: batches
// see the tree as the lower layer of an overlay (writes go to a per-item upper layer), or copy it.
// Entries no batch holds are removed after a while, and all of them when the daemon restarts: a
// hard link keeps its blob's bytes on disk even after CAS garbage collection deletes the blob.

#include "store/merkle.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace zeromesh::exec {

class InputCache {
public:
    // Clears `dir` (anything there is from an earlier daemon run).
    InputCache(std::filesystem::path dir, std::shared_ptr<store::MerkleBuilder> merkle);

    // Holds an entry: it stays on disk at least as long as the lease lives.
    class Lease {
    public:
        Lease(InputCache* cache, std::string key, std::filesystem::path path)
            : cache_(cache), key_(std::move(key)), path_(std::move(path)) {}
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    private:
        InputCache* cache_;
        std::string key_;
        std::filesystem::path path_;
    };

    // The materialized tree `root` (an empty directory for an empty hash); nullptr when a blob or
    // manifest is missing. Concurrent calls for one tree materialize it once.
    std::shared_ptr<Lease> Acquire(const store::Digest& root);

    // Removes entries no lease holds that were last used longer ago than `idle`.
    void Prune(std::chrono::seconds idle);

    [[nodiscard]] size_t Entries() const;

private:
    struct Entry {
        int leases{0};
        bool ready{false};
        bool materializing{false};
        std::chrono::steady_clock::time_point last_used{};
    };

    void Release(const std::string& key);

    std::filesystem::path dir_;
    std::shared_ptr<store::MerkleBuilder> merkle_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, Entry> entries_;
};

} // namespace zeromesh::exec
