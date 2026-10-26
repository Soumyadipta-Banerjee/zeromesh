#ifndef ZMDSM_H
#define ZMDSM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file zmdsm.h
 * @brief ZeroMesh Distributed Shared Memory (DSM) C Library API.
 * Provides virtual shared memory regions over the local network with
 * lazy page-fault invalidation, twin-page diffing, and release-consistency barriers.
 */

// Opaque context for a DSM cluster session
typedef struct zmdsm_ctx zmdsm_ctx_t;

// Access permission flags
typedef enum {
    ZMDSM_READ_ONLY  = 0x1,
    ZMDSM_READ_WRITE = 0x2
} zmdsm_perm_t;

/**
 * @brief Initialize participation in a distributed shared memory mesh cluster.
 * @param cluster_id Mesh cluster identifier (e.g., "sim_cluster_alpha")
 * @param node_id Unique node index (0 to N-1), or -1 for auto-discovery
 * @return Context pointer, or NULL on failure
 */
zmdsm_ctx_t *zmdsm_init(const char *cluster_id, int node_id);

/**
 * @brief Returns total active nodes in the DSM cluster.
 */
int zmdsm_get_cluster_size(zmdsm_ctx_t *ctx);

/**
 * @brief Returns current node's rank / index in the cluster.
 */
int zmdsm_get_node_id(zmdsm_ctx_t *ctx);

/**
 * @brief Allocates a named distributed shared memory region accessible to all nodes.
 * Uses mmap + userfaultfd/mprotect for lazy page fetching across peers.
 * @param ctx DSM context
 * @param region_name Unique global identifier for this memory region
 * @param size_bytes Size of the memory block (rounded up to 4KB page size)
 * @param perm Read/Write access mode
 * @return Virtual address pointer to the shared region
 */
void *zmdsm_alloc(zmdsm_ctx_t *ctx, const char *region_name, size_t size_bytes, zmdsm_perm_t perm);

/**
 * @brief Frees a distributed shared memory segment.
 */
int zmdsm_free(zmdsm_ctx_t *ctx, void *ptr);

/**
 * @brief Global cluster barrier: blocks until all participating nodes arrive.
 * Flushes all pending write-diffs across participating nodes.
 */
int zmdsm_barrier(zmdsm_ctx_t *ctx, const char *barrier_id);

/**
 * @brief Distributed mutex lock on a shared tag.
 */
int zmdsm_lock(zmdsm_ctx_t *ctx, const char *lock_id);

/**
 * @brief Distributed mutex unlock. Propagates dirty pages associated with critical section.
 */
int zmdsm_unlock(zmdsm_ctx_t *ctx, const char *lock_id);

/**
 * @brief Explicitly flushes dirty ranges of a shared buffer to network peers.
 */
int zmdsm_sync(zmdsm_ctx_t *ctx, void *ptr, size_t offset, size_t length);

/**
 * @brief Teardowns and detaches from the DSM cluster session.
 */
void zmdsm_destroy(zmdsm_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // ZMDSM_H
