#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace zeromesh::dsm {

/**
 * @brief Memory Region descriptor managed by the Distributed Shared Memory engine.
 */
struct DsmRegion {
    std::string name;
    void* base_address{nullptr};
    size_t size_bytes{0};
    uint32_t page_count{0};
    bool is_writable{true};
};

/**
 * @brief ZeroMesh Distributed Shared Memory (DSM) Engine.
 * Implements Lazy Release Consistency (LRC) over TCP/QUIC, capturing page writes
 * using userfaultfd or mprotect+SIGSEGV, and propagating bitwise twin diffs
 * across cluster nodes during synchronization fences.
 */
class DsmEngine {
public:
    explicit DsmEngine(std::string cluster_id, int node_rank = -1);
    ~DsmEngine();

    // Non-copyable
    DsmEngine(const DsmEngine&) = delete;
    DsmEngine& operator=(const DsmEngine&) = delete;

    /**
     * @brief Joins the active DSM cluster mesh.
     */
    bool Initialize();

    /**
     * @brief Allocates a named virtual memory region synchronized across all nodes.
     */
    void* AllocateRegion(std::string_view name, size_t size_bytes, bool writable = true);

    /**
     * @brief Releases an allocated distributed shared memory segment.
     */
    bool FreeRegion(void* ptr);

    /**
     * @brief Global barrier synchronization: blocks until all cluster peers check in,
     * flushing all dirty page diffs.
     */
    bool Barrier(std::string_view barrier_id);

    /**
     * @brief Acquires distributed exclusive lock on a named mutex tag.
     */
    bool Lock(std::string_view lock_id);

    /**
     * @brief Releases distributed mutex and pushes dirty pages out to mesh peers.
     */
    bool Unlock(std::string_view lock_id);

    /**
     * @brief Returns the local node's rank / ID.
     */
    [[nodiscard]] int GetRank() const;

    /**
     * @brief Returns total active nodes in the DSM cluster.
     */
    [[nodiscard]] int GetClusterSize() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeromesh::dsm
