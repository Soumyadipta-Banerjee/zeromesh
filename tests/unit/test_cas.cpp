#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "store/store.hpp"
#include "zeromesh/v1/common.pb.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>
#include <vector>

TEST(CasComputeDigest_KnownVectors) {
    // Standard NIST SHA-256 test vector for empty string:
    // e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    auto empty_d = zeromesh::store::CasStore::ComputeDigest("");
    ASSERT_EQ(empty_d.hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ASSERT_EQ(empty_d.size_bytes, 0);

    // Test vector for "hello world"
    // b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
    auto hw_d = zeromesh::store::CasStore::ComputeDigest("hello world");
    ASSERT_EQ(hw_d.hash, "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
    ASSERT_EQ(hw_d.size_bytes, 11);
}

TEST(CasStore_PutAndGetBlob) {
    auto tmp_cas = std::filesystem::temp_directory_path() / "zm_test_cas_store";
    std::filesystem::remove_all(tmp_cas);

    zeromesh::store::CasStore store(tmp_cas);

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

    zeromesh::store::CasStore store(tmp_cas);

    std::string action_hash = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
    std::string action_result_data = "exit_code:0;stdout:compiled successfully";

    ASSERT_TRUE(store.PutActionResult(action_hash, action_result_data));

    auto retrieved_result = store.GetActionResult(action_hash);
    ASSERT_TRUE(retrieved_result.has_value());
    ASSERT_EQ(*retrieved_result, action_result_data);

    ASSERT_FALSE(store.GetActionResult("missing_action_0000000000000000000000000000000000000000").has_value());

    std::filesystem::remove_all(tmp_cas);
}

#include "tests/unit/test_util.hpp"

TEST(CasStore_ConcurrentPutBlob) {
    zeromesh::testing::TempDir d;
    zeromesh::store::CasStore store(d / "cas");
    const std::string payload = "shared blob payload for concurrent writers";
    const auto expected = zeromesh::store::CasStore::ComputeDigest(payload);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            try {
                auto got = store.PutBlob(payload);
                if (got != expected) ++failures;
            } catch (...) {
                ++failures;
            }
        });
    }
    for (auto& t : threads) t.join();
    ASSERT_EQ(failures.load(), 0);
    ASSERT_TRUE(store.HasBlob(expected.hash));
    ASSERT_EQ(store.GetBlob(expected.hash), std::optional<std::string>(payload));
}

TEST(CasRejectsNonDigestHash) {
    zeromesh::testing::TempDir d;
    zeromesh::store::CasStore cas(d / "cas");
    zeromesh::testing::WriteFile(d / "secret", "s");
    ASSERT_FALSE(cas.GetBlob("../../secret").has_value());
    ASSERT_FALSE(cas.HasBlob("../../secret"));
    ASSERT_FALSE(cas.GetActionResult("../secret").has_value());
    ASSERT_FALSE(cas.PutActionResult("../x", "r"));
    ASSERT_FALSE(std::filesystem::exists(d / "x"));
}

TEST(CasCollectGarbage_EvictsLruUnprotected) {
    zeromesh::testing::TempDir d;
    zeromesh::store::CasLimits limits;
    limits.max_blob_bytes = 1024;
    zeromesh::store::CasStore store(d / "cas", limits);

    const auto older = store.PutBlob(std::string(80, 'o'));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto newer = store.PutBlob(std::string(80, 'n'));

    ASSERT_TRUE(store.GetBlob(older.hash).has_value());

    std::unordered_set<std::string> protect{older.hash};
    const auto stats = store.CollectGarbage(0, protect);
    ASSERT_EQ(stats.blobs_removed, 1u);
    ASSERT_TRUE(store.HasBlob(older.hash));
    ASSERT_FALSE(store.HasBlob(newer.hash));
}

TEST(CasHasBlob_FalseWhenFileRemovedDespiteMemCache) {
    zeromesh::testing::TempDir d;
    zeromesh::store::CasStore store(d / "cas");
    const std::string payload = "cached-small-blob";
    const auto digest = store.PutBlob(payload);
    ASSERT_TRUE(store.HasBlob(digest.hash));
    std::filesystem::remove(store.GetBlobPath(digest.hash));
    ASSERT_FALSE(store.HasBlob(digest.hash));
}

TEST(CasCollectGarbage_ProtectedSurvive) {
    zeromesh::testing::TempDir d;
    zeromesh::store::CasLimits limits;
    limits.max_blob_bytes = 4096;
    zeromesh::store::CasStore store(d / "cas", limits);

    const auto keep = store.PutBlob("keep-me");
    const auto drop = store.PutBlob("drop-me");

    std::unordered_set<std::string> protect{keep.hash};
    store.CollectGarbage(0, protect);

    ASSERT_TRUE(store.HasBlob(keep.hash));
    ASSERT_FALSE(store.HasBlob(drop.hash));
}

TEST(CasQuota_ReserveIncomingRejectsWhenFull) {
    zeromesh::testing::TempDir d;
    zeromesh::store::CasLimits limits;
    limits.max_blob_bytes = 200;
    zeromesh::store::CasStore store(d / "cas", limits);

    store.PutBlob(std::string(120, 'x'));
    ASSERT_FALSE(store.ReserveIncoming(100));
    ASSERT_TRUE(store.ReserveIncoming(50));
    store.ReleaseIncoming(50);
}

TEST(CasConcurrentPutAndGc_Integrity) {
    zeromesh::testing::TempDir d;
    zeromesh::store::CasLimits limits;
    limits.max_blob_bytes = 64 * 1024;
    zeromesh::store::CasStore store(d / "cas", limits);

    std::atomic<bool> stop{false};
    std::atomic<int> integrity_failures{0};

    std::thread gc_thread([&] {
        while (!stop.load()) {
            store.CollectGarbage(store.BlobBytes() * 3 / 4, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (int i = 0; i < 40; ++i) {
        const std::string payload = "payload-" + std::to_string(i) + "-" + std::string(64, 'p');
        const auto digest = store.PutBlob(payload);
        // Unprotected blobs may vanish under concurrent GC; that is not corruption.
        // Fail only when a still-present blob has the wrong bytes/hash.
        if (!store.HasBlob(digest.hash)) continue;
        const auto got = store.GetBlob(digest.hash);
        if (!got) continue; // GC raced between HasBlob and GetBlob
        if (*got != payload || zeromesh::store::CasStore::ComputeDigest(*got).hash != digest.hash) {
            ++integrity_failures;
        }
    }

    stop = true;
    gc_thread.join();
    ASSERT_EQ(integrity_failures.load(), 0);
}

namespace {

int64_t UnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ThisBoot() {
    std::ifstream in("/proc/sys/kernel/random/boot_id");
    std::string id;
    std::getline(in, id);
    return id;
}

void SetAge(const std::filesystem::path& p, std::chrono::seconds age) {
    std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now() - age);
}

} // namespace

TEST(CasStoreChecksWhatAMachineThatWentDownWroteAfterItsLastRecord) {
    zeromesh::testing::TempDir d;
    const auto dir = d / "cas";
    const std::string key(64, 'a');
    zeromesh::store::Digest old, good, torn;
    {
        zeromesh::store::CasStore store(dir);
        old = store.PutBlob("written long before");
        good = store.PutBlob("written after the record, intact");
        torn = store.PutBlob("written after the record, torn when the machine went down");
        ASSERT_TRUE(store.PutActionResult(key, "an entry written after the record", true));
        store.Flush();
    }
    SetAge(dir / "blobs" / old.hash, std::chrono::hours(1));
    // The last record is ten minutes old and from another boot; one blob lost its bytes.
    std::ofstream(dir / "durable", std::ios::trunc) << "another-boot " << UnixMs() - 600'000 << "\n";
    std::ofstream(dir / "blobs" / torn.hash, std::ios::trunc) << "";

    zeromesh::store::CasStore store(dir);
    ASSERT_TRUE(store.HasBlob(old.hash));        // older than the record: synced then
    ASSERT_TRUE(store.HasBlob(good.hash));       // re-hashed and kept
    ASSERT_FALSE(store.HasBlob(torn.hash));      // re-hashed and dropped
    ASSERT_FALSE(store.GetActionResult(key));    // cannot be checked: dropped
    std::ifstream record(dir / "durable");
    std::string boot;
    record >> boot;
    ASSERT_EQ(boot, ThisBoot()); // this run's writes are covered from now on
}

TEST(CasStoreChecksNothingAfterARestartInTheSameBoot) {
    zeromesh::testing::TempDir d;
    const auto dir = d / "cas";
    zeromesh::store::Digest blob;
    {
        zeromesh::store::CasStore store(dir);
        blob = store.PutBlob("kept by the kernel across a process restart");
    }
    std::ofstream(dir / "durable", std::ios::trunc) << ThisBoot() << " " << UnixMs() - 600'000 << "\n";
    std::ofstream(dir / "blobs" / blob.hash, std::ios::app) << "x"; // would fail a check
    zeromesh::store::CasStore store(dir);
    ASSERT_TRUE(store.HasBlob(blob.hash)); // not re-hashed: nothing was lost
}

TEST(CasPresenceChecksStampABlobAtMostOncePerInterval) {
    zeromesh::testing::TempDir d;
    const auto dir = d / "cas";
    zeromesh::store::Digest blob;
    {
        zeromesh::store::CasStore store(dir);
        blob = store.PutBlob("checked often");
    }
    const auto path = dir / "blobs" / blob.hash;
    SetAge(path, std::chrono::hours(1));
    zeromesh::store::CasStore store(dir); // its recency, from the mtime, is an hour old
    ASSERT_TRUE(store.HasBlob(blob.hash));
    ASSERT_TRUE(std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(path) <
                std::chrono::minutes(1)); // stamped once
    SetAge(path, std::chrono::hours(1));
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(store.HasBlob(blob.hash));
    ASSERT_TRUE(std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(path) >
                std::chrono::minutes(30)); // and not again within the interval
}
