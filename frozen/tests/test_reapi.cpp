#include "tests/test_framework.hpp"
#include "cas/store.hpp"
#include "reapi/action_cache.hpp"
#include "reapi/cas_service.hpp"
#include "reapi/execution_service.hpp"
#include "scheduler/scheduler.hpp"

#include <filesystem>
#include <memory>

TEST(ReapiCasService_FindMissingAndBatchUpdate) {
    auto tmp_cas = std::filesystem::temp_directory_path() / "zm_test_reapi_cas";
    std::filesystem::remove_all(tmp_cas);

    auto store = std::make_shared<zeromesh::cas::CasStore>(tmp_cas);
    zeromesh::reapi::CasService cas_svc(store);

    zeromesh::cas::Digest d1{"hash_alpha", 100};
    zeromesh::cas::Digest d2{"hash_beta", 200};

    // Both should be missing initially
    auto missing = cas_svc.FindMissingBlobs({d1, d2});
    ASSERT_EQ(missing.size(), 2);

    // Update d1 into CAS
    std::string data1 = "payload for alpha";
    auto computed_d1 = zeromesh::cas::CasStore::ComputeDigest(data1);
    ASSERT_TRUE(cas_svc.BatchUpdateBlobs({{computed_d1, data1}}));

    // Now computed_d1 is present, d2 is still missing
    missing = cas_svc.FindMissingBlobs({computed_d1, d2});
    ASSERT_EQ(missing.size(), 1);
    ASSERT_EQ(missing[0].hash, d2.hash);

    // Read back computed_d1
    auto read_blobs = cas_svc.BatchReadBlobs({computed_d1});
    ASSERT_EQ(read_blobs.size(), 1);
    ASSERT_EQ(read_blobs[0].second, data1);

    std::filesystem::remove_all(tmp_cas);
}

TEST(ReapiExecutionService_CacheFirstShortCircuit) {
    auto tmp_cas = std::filesystem::temp_directory_path() / "zm_test_reapi_exec";
    std::filesystem::remove_all(tmp_cas);

    auto store = std::make_shared<zeromesh::cas::CasStore>(tmp_cas);
    auto ac_svc = std::make_shared<zeromesh::reapi::ActionCacheService>(store);
    auto peer_table = std::make_shared<zeromesh::discovery::PeerTable>();
    auto scheduler = std::make_shared<zeromesh::scheduler::Scheduler>(peer_table);

    zeromesh::reapi::ExecutionService exec_svc(store, ac_svc, scheduler, nullptr);

    zeromesh::executor::ExecutionRequest req{
        .action_id = "action-sha256-test-unique-999",
        .command_args = {"g++", "-c", "test.cpp", "-o", "test.o"},
        .env_vars = {},
        .input_root_digest = zeromesh::cas::Digest{"", 0},
        .expected_output_files = {"test.o"},
        .expected_output_directories = {},
        .working_directory = "."
    };

    // 1st Execution: Cache Miss -> Executes through pipeline and populates ActionCache
    auto res1 = exec_svc.ExecuteAction(req);
    ASSERT_EQ(res1.exit_code, 0);
    ASSERT_EQ(res1.worker_id, "local");

    // 2nd Execution: Cache-First Short-Circuit -> Returns instantly with worker_id == "cache-hit"
    auto res2 = exec_svc.ExecuteAction(req);
    ASSERT_EQ(res2.exit_code, 0);
    ASSERT_EQ(res2.worker_id, "cache-hit");

    std::filesystem::remove_all(tmp_cas);
}
