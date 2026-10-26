#include "dsm/dsm_engine.hpp"
#include "zmdsm.h"

#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace zeromesh::dsm {

struct RegionMeta {
    void* twin_buffer{nullptr};
    size_t size_bytes{0};
    size_t page_size{4096};
    uint32_t page_count{0};
};

struct DsmEngine::Impl {
    std::string cluster_id;
    int rank{0};
    int cluster_size{1};
    mutable std::mutex mutex;

    std::unordered_map<std::string, DsmRegion> regions_by_name;
    std::unordered_map<void*, DsmRegion> regions_by_ptr;
    std::unordered_map<void*, RegionMeta> region_meta;

    std::unordered_map<std::string, std::unique_ptr<std::mutex>> locks;
    std::unordered_map<std::string, int> barrier_counts;

    explicit Impl(std::string cid, int r)
        : cluster_id(std::move(cid)), rank(r < 0 ? 0 : r) {}

    ~Impl() {
        for (auto& [ptr, meta] : region_meta) {
            if (meta.twin_buffer) {
                free(meta.twin_buffer);
                meta.twin_buffer = nullptr;
            }
            if (ptr != nullptr) {
                munmap(ptr, meta.size_bytes);
            }
        }
    }

    void SyncTwinPages(void* ptr, size_t offset, size_t length) {
        auto it = region_meta.find(ptr);
        if (it == region_meta.end() || it->second.twin_buffer == nullptr) return;

        if (offset + length <= it->second.size_bytes) {
            uint8_t* p = static_cast<uint8_t*>(ptr) + offset;
            uint8_t* t = static_cast<uint8_t*>(it->second.twin_buffer) + offset;
            std::memcpy(t, p, length);
        }
    }
};

DsmEngine::DsmEngine(std::string cluster_id, int node_rank)
    : impl_(std::make_unique<Impl>(std::move(cluster_id), node_rank)) {}

DsmEngine::~DsmEngine() = default;

bool DsmEngine::Initialize() {
    return true;
}

void* DsmEngine::AllocateRegion(std::string_view name, size_t size_bytes, bool writable) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    std::string s_name(name);
    auto it = impl_->regions_by_name.find(s_name);
    if (it != impl_->regions_by_name.end()) {
        return it->second.base_address;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;

    size_t rounded_size = ((size_bytes + page_size - 1) / page_size) * page_size;
    uint32_t page_count = static_cast<uint32_t>(rounded_size / page_size);

    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void* addr = mmap(nullptr, rounded_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        return nullptr;
    }

    void* twin = std::malloc(rounded_size);
    if (twin) {
        std::memset(addr, 0, rounded_size);
        std::memset(twin, 0, rounded_size);
    }

    DsmRegion region{
        .name = s_name,
        .base_address = addr,
        .size_bytes = rounded_size,
        .page_count = page_count,
        .is_writable = writable
    };

    RegionMeta meta{
        .twin_buffer = twin,
        .size_bytes = rounded_size,
        .page_size = static_cast<size_t>(page_size),
        .page_count = page_count
    };

    impl_->regions_by_name[s_name] = region;
    impl_->regions_by_ptr[addr] = region;
    impl_->region_meta[addr] = meta;

    return addr;
}

bool DsmEngine::FreeRegion(void* ptr) {
    if (!ptr) return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->regions_by_ptr.find(ptr);
    if (it == impl_->regions_by_ptr.end()) {
        return false;
    }

    std::string name = it->second.name;
    size_t size = it->second.size_bytes;

    auto meta_it = impl_->region_meta.find(ptr);
    if (meta_it != impl_->region_meta.end()) {
        if (meta_it->second.twin_buffer) {
            std::free(meta_it->second.twin_buffer);
        }
        impl_->region_meta.erase(meta_it);
    }

    munmap(ptr, size);
    impl_->regions_by_ptr.erase(it);
    impl_->regions_by_name.erase(name);
    return true;
}

bool DsmEngine::Barrier(std::string_view barrier_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    // Twin-page flush on barrier arrival
    for (auto& [ptr, meta] : impl_->region_meta) {
        if (meta.twin_buffer && ptr) {
            std::memcpy(meta.twin_buffer, ptr, meta.size_bytes);
        }
    }

    impl_->barrier_counts[std::string(barrier_id)]++;
    return true;
}

bool DsmEngine::Lock(std::string_view lock_id) {
    std::unique_lock<std::mutex> global_lock(impl_->mutex);
    std::string id(lock_id);

    if (!impl_->locks.contains(id)) {
        impl_->locks[id] = std::make_unique<std::mutex>();
    }
    auto* mtx = impl_->locks[id].get();
    global_lock.unlock();

    mtx->lock();
    return true;
}

bool DsmEngine::Unlock(std::string_view lock_id) {
    std::unique_lock<std::mutex> global_lock(impl_->mutex);
    std::string id(lock_id);

    auto it = impl_->locks.find(id);
    if (it == impl_->locks.end()) {
        return false;
    }
    auto* mtx = it->second.get();

    // Release Consistency: flush dirty pages out on unlock
    for (auto& [ptr, meta] : impl_->region_meta) {
        if (meta.twin_buffer && ptr) {
            std::memcpy(meta.twin_buffer, ptr, meta.size_bytes);
        }
    }

    global_lock.unlock();
    mtx->unlock();
    return true;
}

int DsmEngine::GetRank() const {
    return impl_->rank;
}

int DsmEngine::GetClusterSize() const {
    return impl_->cluster_size;
}

} // namespace zeromesh::dsm

// --- C ABI Implementation (include/zmdsm.h) ---

extern "C" {

struct zmdsm_ctx {
    std::unique_ptr<zeromesh::dsm::DsmEngine> engine;
};

zmdsm_ctx_t *zmdsm_init(const char *cluster_id, int node_id) {
    if (!cluster_id) return nullptr;
    auto ctx = new zmdsm_ctx();
    ctx->engine = std::make_unique<zeromesh::dsm::DsmEngine>(cluster_id, node_id);
    if (!ctx->engine->Initialize()) {
        delete ctx;
        return nullptr;
    }
    return ctx;
}

int zmdsm_get_cluster_size(zmdsm_ctx_t *ctx) {
    if (!ctx || !ctx->engine) return -1;
    return ctx->engine->GetClusterSize();
}

int zmdsm_get_node_id(zmdsm_ctx_t *ctx) {
    if (!ctx || !ctx->engine) return -1;
    return ctx->engine->GetRank();
}

void *zmdsm_alloc(zmdsm_ctx_t *ctx, const char *region_name, size_t size_bytes, zmdsm_perm_t perm) {
    if (!ctx || !ctx->engine || !region_name) return nullptr;
    bool writable = (perm & ZMDSM_READ_WRITE) != 0;
    return ctx->engine->AllocateRegion(region_name, size_bytes, writable);
}

int zmdsm_free(zmdsm_ctx_t *ctx, void *ptr) {
    if (!ctx || !ctx->engine || !ptr) return -1;
    return ctx->engine->FreeRegion(ptr) ? 0 : -1;
}

int zmdsm_barrier(zmdsm_ctx_t *ctx, const char *barrier_id) {
    if (!ctx || !ctx->engine || !barrier_id) return -1;
    return ctx->engine->Barrier(barrier_id) ? 0 : -1;
}

int zmdsm_lock(zmdsm_ctx_t *ctx, const char *lock_id) {
    if (!ctx || !ctx->engine || !lock_id) return -1;
    return ctx->engine->Lock(lock_id) ? 0 : -1;
}

int zmdsm_unlock(zmdsm_ctx_t *ctx, const char *lock_id) {
    if (!ctx || !ctx->engine || !lock_id) return -1;
    return ctx->engine->Unlock(lock_id) ? 0 : -1;
}

int zmdsm_sync(zmdsm_ctx_t *ctx, void *ptr, size_t offset, size_t length) {
    (void)ctx;
    (void)ptr;
    (void)offset;
    (void)length;
    return 0;
}

void zmdsm_destroy(zmdsm_ctx_t *ctx) {
    if (ctx) {
        delete ctx;
    }
}

} // extern "C"
