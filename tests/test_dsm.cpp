#include "tests/test_framework.hpp"
#include "dsm/dsm_engine.hpp"
#include "zmdsm.h"

#include <cstring>

TEST(DsmEngine_AllocateAndFree) {
    using namespace zeromesh::dsm;

    DsmEngine engine("cluster_test", 0);
    ASSERT_TRUE(engine.Initialize());
    ASSERT_EQ(engine.GetRank(), 0);

    // Allocate 1000 bytes (will be rounded up to 4096 page boundary)
    void* ptr = engine.AllocateRegion("shared_buffer_1", 1000, true);
    ASSERT_TRUE(ptr != nullptr);

    // Verify 4096-byte page alignment
    ASSERT_EQ(reinterpret_cast<uintptr_t>(ptr) % 4096, 0);

    // Write data to the shared region
    const char* test_str = "ZeroMesh Network DSM Shared Page";
    std::memcpy(ptr, test_str, std::strlen(test_str) + 1);

    // Read back and verify
    ASSERT_EQ(std::strcmp(static_cast<const char*>(ptr), test_str), 0);

    // Free the region
    ASSERT_TRUE(engine.FreeRegion(ptr));
    ASSERT_FALSE(engine.FreeRegion(ptr)); // double free returns false
}

TEST(DsmEngine_MultipleRegionsAndLocks) {
    using namespace zeromesh::dsm;

    DsmEngine engine("cluster_test", 1);
    ASSERT_TRUE(engine.Initialize());

    void* r1 = engine.AllocateRegion("region_alpha", 4096, true);
    void* r2 = engine.AllocateRegion("region_beta", 8192, true);

    ASSERT_TRUE(r1 != nullptr);
    ASSERT_TRUE(r2 != nullptr);
    ASSERT_NE(r1, r2);

    int* arr1 = static_cast<int*>(r1);
    int* arr2 = static_cast<int*>(r2);

    arr1[0] = 42;
    arr2[0] = 84;

    ASSERT_EQ(arr1[0], 42);
    ASSERT_EQ(arr2[0], 84);

    // Test distributed lock / unlock
    ASSERT_TRUE(engine.Lock("mutex_tag_1"));
    arr1[0] += 1;
    ASSERT_TRUE(engine.Unlock("mutex_tag_1"));
    ASSERT_EQ(arr1[0], 43);

    // Test global barrier
    ASSERT_TRUE(engine.Barrier("epoch_barrier_1"));

    engine.FreeRegion(r1);
    engine.FreeRegion(r2);
}

TEST(C_ABI_zmdsm_FullLifecycle) {
    // Test pure C ABI interface for third-party programs
    zmdsm_ctx_t* ctx = zmdsm_init("c_api_cluster", 2);
    ASSERT_TRUE(ctx != nullptr);

    ASSERT_EQ(zmdsm_get_node_id(ctx), 2);
    ASSERT_TRUE(zmdsm_get_cluster_size(ctx) >= 1);

    void* mem = zmdsm_alloc(ctx, "c_shared_block", 4096, ZMDSM_READ_WRITE);
    ASSERT_TRUE(mem != nullptr);

    uint32_t* values = static_cast<uint32_t*>(mem);
    for (int i = 0; i < 100; ++i) {
        values[i] = i * 10;
    }

    ASSERT_EQ(zmdsm_lock(ctx, "c_lock"), 0);
    values[0] = 9999;
    ASSERT_EQ(zmdsm_unlock(ctx, "c_lock"), 0);

    ASSERT_EQ(values[0], 9999);
    ASSERT_EQ(values[50], 500);

    ASSERT_EQ(zmdsm_barrier(ctx, "c_barrier"), 0);
    ASSERT_EQ(zmdsm_free(ctx, mem), 0);

    zmdsm_destroy(ctx);
}
