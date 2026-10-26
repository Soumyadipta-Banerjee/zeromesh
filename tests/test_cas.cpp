#include "tests/test_framework.hpp"
#include "cas/store.hpp"

#include <filesystem>

TEST(CasComputeDigest_KnownVectors) {
    // Standard NIST SHA-256 test vector for empty string:
    // e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    auto empty_d = zeromesh::cas::CasStore::ComputeDigest("");
    ASSERT_EQ(empty_d.hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ASSERT_EQ(empty_d.size_bytes, 0);

    // Test vector for "hello world"
    // b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
    auto hw_d = zeromesh::cas::CasStore::ComputeDigest("hello world");
    ASSERT_EQ(hw_d.hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    ASSERT_EQ(hw_d.size_bytes, 11);
}

TEST(CasStore_PutAndGetBlob) {
    auto tmp_cas = std::filesystem::temp_directory_path() / "zm_test_cas_store";
    std::filesystem::remove_all(tmp_cas);

    zeromesh::cas::CasStore store(tmp_cas);

    std::string sample_data = "int main() { return 0; }";
    auto digest = store.PutBlob(sample_data);

    ASSERT_TRUE(store.HasBlob(digest.hash));

    auto retrieved = store.GetBlob(digest.hash);
    ASSERT_TRUE(retrieved.has_value());
    ASSERT_EQ(*retrieved, sample_data);

    ASSERT_FALSE(store.HasBlob("nonexistent_hash_00000000000000000000000000000000000000000000000"));
    ASSERT_FALSE(store.GetBlob("nonexistent_hash_00000000000000000000000000000000000000000000000").has_value());

    std::filesystem::remove_all(tmp_cas);
}

TEST(CasStore_ActionCachePutAndGet) {
    auto tmp_cas = std::filesystem::temp_directory_path() / "zm_test_cas_ac";
    std::filesystem::remove_all(tmp_cas);

    zeromesh::cas::CasStore store(tmp_cas);

    std::string action_hash = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
    std::string action_result_data = "exit_code:0;stdout:compiled successfully";

    ASSERT_TRUE(store.PutActionResult(action_hash, action_result_data));

    auto retrieved_result = store.GetActionResult(action_hash);
    ASSERT_TRUE(retrieved_result.has_value());
    ASSERT_EQ(*retrieved_result, action_result_data);

    ASSERT_FALSE(store.GetActionResult("missing_action_0000000000000000000000000000000000000000").has_value());

    std::filesystem::remove_all(tmp_cas);
}
