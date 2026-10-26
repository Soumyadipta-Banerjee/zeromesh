#include "tests/test_framework.hpp"
#include "vfs/fuse_fs.hpp"
#include "vfs/chunk_fetcher.hpp"
#include "cas/store.hpp"
#include "dht/kademlia.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace zeromesh;
using namespace zeromesh::vfs;

TEST(ChunkCache_PutGetAndEvict) {
    auto tmp_cache = std::filesystem::temp_directory_path() / "zm_test_vfs_chunk_cache";
    std::filesystem::remove_all(tmp_cache);

    // Limit cache to 1000 bytes so we can test eviction easily
    ChunkCache cache(tmp_cache, 1000);

    std::string hash1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    std::string data1(400, 'A');
    std::string hash2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    std::string data2(400, 'B');
    std::string hash3 = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    std::string data3(400, 'C');

    cache.Put(hash1, data1);
    ASSERT_TRUE(cache.Contains(hash1));
    auto got1 = cache.Get(hash1);
    ASSERT_TRUE(got1.has_value());
    ASSERT_EQ(*got1, data1);

    cache.Put(hash2, data2);
    ASSERT_TRUE(cache.Contains(hash2));
    ASSERT_EQ(cache.CurrentBytes(), 800);

    // Inserting data3 (400 bytes) will exceed 1000 bytes (800+400 = 1200 > 1000)
    // Oldest entry (hash1) should be evicted
    cache.Put(hash3, data3);
    ASSERT_TRUE(cache.Contains(hash3));
    ASSERT_TRUE(cache.Contains(hash2));
    ASSERT_FALSE(cache.Contains(hash1));
    ASSERT_TRUE(cache.EvictionCount() >= 1);

    std::filesystem::remove_all(tmp_cache);
}

TEST(FuseFileSystem_PublishFileAndLookup) {
    auto tmp_root = std::filesystem::temp_directory_path() / "zm_test_vfs_publish";
    std::filesystem::remove_all(tmp_root);

    auto cas_dir = tmp_root / "cas";
    auto cache_dir = tmp_root / "cache";
    auto src_dir = tmp_root / "src";
    std::filesystem::create_directories(src_dir);

    // Create a dummy file to publish
    auto test_file = src_dir / "sample.txt";
    std::string content = "ZeroMesh distributed filesystem backed by Kademlia DHT and CAS.";
    {
        std::ofstream ofs(test_file);
        ofs << content;
    }

    auto cas_store = std::make_shared<cas::CasStore>(cas_dir);
    
    dht::NodeId self_id{};
    self_id.fill(0x01);
    auto dht = std::make_shared<dht::KademliaDht>(self_id, 19890);

    FuseConfig fuse_cfg;
    fuse_cfg.cache_dir = cache_dir;
    fuse_cfg.chunk_size_bytes = 16; // Small 16-byte chunks to test multi-chunk splitting
    FuseFileSystem vfs(fuse_cfg, dht, cas_store);

    std::string manifest_hash = vfs.PublishFile(test_file, "/docs/sample.txt");
    ASSERT_FALSE(manifest_hash.empty());

    // Verify lookup by virtual path
    auto inode = vfs.Lookup("/docs/sample.txt");
    ASSERT_TRUE(inode.has_value());
    ASSERT_EQ(inode->size_bytes, content.size());
    ASSERT_EQ(inode->type, InodeType::RegularFile);

    // Verify chunks were created and put into CAS
    size_t expected_chunks = (content.size() + 15) / 16;
    ASSERT_EQ(inode->chunk_hashes.size(), expected_chunks);

    for (const auto& ch_hash : inode->chunk_hashes) {
        ASSERT_TRUE(cas_store->HasBlob(ch_hash));
    }

    // Verify directory hierarchy
    auto root_entries = vfs.ReadDir("/");
    ASSERT_FALSE(root_entries.empty());
    bool found_docs = false;
    for (const auto& entry : root_entries) {
        if (entry.name == "docs") {
            found_docs = true;
            ASSERT_EQ(entry.type, InodeType::Directory);
        }
    }
    ASSERT_TRUE(found_docs);

    auto docs_entries = vfs.ReadDir("/docs");
    ASSERT_FALSE(docs_entries.empty());
    bool found_sample = false;
    for (const auto& entry : docs_entries) {
        if (entry.name == "sample.txt") {
            found_sample = true;
            ASSERT_EQ(entry.type, InodeType::RegularFile);
        }
    }
    ASSERT_TRUE(found_sample);

    std::filesystem::remove_all(tmp_root);
}

TEST(FuseFileSystem_PublishDirectory) {
    auto tmp_root = std::filesystem::temp_directory_path() / "zm_test_vfs_pubdir";
    std::filesystem::remove_all(tmp_root);

    auto cas_dir = tmp_root / "cas";
    auto src_dir = tmp_root / "test_repo";
    std::filesystem::create_directories(src_dir / "subdir");

    {
        std::ofstream ofs(src_dir / "file1.txt");
        ofs << "file one content";
    }
    {
        std::ofstream ofs(src_dir / "subdir" / "file2.txt");
        ofs << "file two content in nested dir";
    }

    auto cas_store = std::make_shared<cas::CasStore>(cas_dir);

    dht::NodeId self_id{};
    self_id.fill(0x02);
    auto dht = std::make_shared<dht::KademliaDht>(self_id, 19891);

    FuseConfig fuse_cfg;
    fuse_cfg.cache_dir = tmp_root / "cache";
    FuseFileSystem vfs(fuse_cfg, dht, cas_store);

    size_t published = vfs.PublishDirectory(src_dir, "/repo");
    ASSERT_EQ(published, 2);

    auto file1_inode = vfs.Lookup("/repo/file1.txt");
    ASSERT_TRUE(file1_inode.has_value());

    auto file2_inode = vfs.Lookup("/repo/subdir/file2.txt");
    ASSERT_TRUE(file2_inode.has_value());

    std::filesystem::remove_all(tmp_root);
}
