#include "tests/test_framework.hpp"
#include "tests/examples/torrent_engine.hpp"
#include "tests/examples/torrent_manifest.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace zeromesh;
using namespace zeromesh::examples;

TEST(TorrentEngine_ExportAndDownloadSwarm) {
    auto test_root = std::filesystem::temp_directory_path() / "zm_test_torrent_swarm";
    std::filesystem::remove_all(test_root);

    auto src_dir = test_root / "node_a_shared_folder";
    auto node_a_storage = test_root / "node_a_storage";
    auto node_b_storage = test_root / "node_b_storage";
    auto node_b_dest = test_root / "node_b_downloads";

    std::filesystem::create_directories(src_dir / "subdir");
    std::filesystem::create_directories(node_b_dest);

    // Create test files in node A's directory
    std::string content1 = "ZeroMesh distributed torrent file sharing engine - File 1 content.";
    std::string content2 = "Second file in nested directory with some more bytes to chunk.";

    {
        std::ofstream ofs(src_dir / "hello.txt");
        ofs << content1;
    }
    {
        std::ofstream ofs(src_dir / "subdir" / "nested.txt");
        ofs << content2;
    }

    // Start Node A (Seeder)
    uint16_t dht_port_a = 19910;
    uint16_t chunk_port_a = 19914;
    TorrentEngine node_a(node_a_storage, dht_port_a, chunk_port_a);
    ASSERT_TRUE(node_a.Start());

    // Start Node B (Leecher / Downloader)
    uint16_t dht_port_b = 19920;
    uint16_t chunk_port_b = 19924;
    TorrentEngine node_b(node_b_storage, dht_port_b, chunk_port_b);
    ASSERT_TRUE(node_b.Start());

    // Interconnect Node A and Node B in DHT routing table
    node_a.AddDhtPeer("127.0.0.1", dht_port_b, node_b.GetStats().self_node_id);
    node_b.AddDhtPeer("127.0.0.1", dht_port_a, node_a.GetStats().self_node_id);

    // Export directory on Node A (small 32-byte chunks to test multi-chunk distribution)
    auto [manifest, info_hash] = node_a.ExportDirectory(src_dir, 32);
    ASSERT_FALSE(info_hash.empty());
    ASSERT_EQ(manifest.files.size(), 2);

    std::string magnet = TorrentManifest::MakeMagnetUri(info_hash, manifest.name, manifest.total_bytes);
    ASSERT_FALSE(magnet.empty());

    auto extracted_hash = TorrentManifest::ExtractHash(magnet);
    ASSERT_TRUE(extracted_hash.has_value());
    ASSERT_EQ(*extracted_hash, info_hash);

    // Node B downloads using only the info_hash
    auto download_res = node_b.DownloadTorrent(info_hash, node_b_dest);
    if (!download_res) {
        std::cerr << ">>> DownloadTorrent failed: " << download_res.error_message << std::endl;
    }
    ASSERT_TRUE(download_res.success);

    // Verify downloaded directory and file contents on Node B
    auto downloaded_dir = node_b_dest / manifest.name;
    ASSERT_TRUE(std::filesystem::exists(downloaded_dir / "hello.txt"));
    ASSERT_TRUE(std::filesystem::exists(downloaded_dir / "subdir" / "nested.txt"));

    std::string downloaded_c1;
    {
        std::ifstream ifs(downloaded_dir / "hello.txt");
        downloaded_c1.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    ASSERT_EQ(downloaded_c1, content1);

    std::string downloaded_c2;
    {
        std::ifstream ifs(downloaded_dir / "subdir" / "nested.txt");
        downloaded_c2.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    ASSERT_EQ(downloaded_c2, content2);

    node_a.Stop();
    node_b.Stop();
    std::filesystem::remove_all(test_root);
}

TEST(TorrentEngine_LiveSyncUpdatesCreatesAndDeletes) {
    auto test_root = std::filesystem::temp_directory_path() / "zm_test_torrent_livesync";
    std::filesystem::remove_all(test_root);

    auto dir_a = test_root / "seeder_a_folder";
    auto dir_b_parent = test_root / "seeder_b_parent";
    auto storage_a = test_root / "storage_a";
    auto storage_b = test_root / "storage_b";

    std::filesystem::create_directories(dir_a / "sub");
    std::filesystem::create_directories(dir_b_parent);

    {
        std::ofstream ofs(dir_a / "file1.txt");
        ofs << "Initial File 1";
    }
    {
        std::ofstream ofs(dir_a / "sub" / "file2.txt");
        ofs << "Initial File 2";
    }

    uint16_t dht_port_a = 19930;
    uint16_t chunk_port_a = 19934;
    TorrentEngine node_a(storage_a, dht_port_a, chunk_port_a);
    ASSERT_TRUE(node_a.Start());

    uint16_t dht_port_b = 19940;
    uint16_t chunk_port_b = 19944;
    TorrentEngine node_b(storage_b, dht_port_b, chunk_port_b);
    ASSERT_TRUE(node_b.Start());

    node_a.AddDhtPeer("127.0.0.1", dht_port_b, node_b.GetStats().self_node_id, chunk_port_b);
    node_b.AddDhtPeer("127.0.0.1", dht_port_a, node_a.GetStats().self_node_id, chunk_port_a);

    // Initial Export from Node A
    auto [manifest, info_hash] = node_a.ExportDirectory(dir_a, 64);
    ASSERT_TRUE(node_a.IsLiveSyncActive());
    ASSERT_EQ(node_a.GetLiveSyncVersion(), 1);

    // Download to Node B
    auto download_res = node_b.DownloadTorrent(info_hash, dir_b_parent, nullptr, "127.0.0.1", dht_port_a, chunk_port_a);
    ASSERT_TRUE(download_res.success);
    ASSERT_TRUE(node_b.IsLiveSyncActive());
    ASSERT_EQ(node_b.GetLiveSyncVersion(), 1);

    auto dir_b = dir_b_parent / manifest.name;
    ASSERT_TRUE(std::filesystem::exists(dir_b / "file1.txt"));
    ASSERT_TRUE(std::filesystem::exists(dir_b / "sub" / "file2.txt"));

    // ─────────────────────────────────────────────────────────────────────────
    // Phase 1: Create a new file on Node A, verify Node B receives it
    // ─────────────────────────────────────────────────────────────────────────
    std::string new_file_content = "Created on Node A at runtime!";
    {
        std::ofstream ofs(dir_a / "created_by_a.txt");
        ofs << new_file_content;
    }

    // Trigger Node A local check
    ASSERT_TRUE(node_a.SyncTick());
    ASSERT_EQ(node_a.GetLiveSyncVersion(), 2);

    // Trigger Node B remote check
    ASSERT_TRUE(node_b.SyncTick());
    ASSERT_EQ(node_b.GetLiveSyncVersion(), 2);

    // Verify file appeared on Node B
    ASSERT_TRUE(std::filesystem::exists(dir_b / "created_by_a.txt"));
    std::string read_b;
    {
        std::ifstream ifs(dir_b / "created_by_a.txt");
        read_b.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    ASSERT_EQ(read_b, new_file_content);

    // ─────────────────────────────────────────────────────────────────────────
    // Phase 2: Modify file1 and delete sub/file2 on Node B, verify Node A receives it
    // ─────────────────────────────────────────────────────────────────────────
    std::string updated_f1 = "Modified on Node B!";
    {
        std::ofstream ofs(dir_b / "file1.txt");
        ofs << updated_f1;
    }
    std::filesystem::remove(dir_b / "sub" / "file2.txt");

    // Trigger Node B local check
    ASSERT_TRUE(node_b.SyncTick());
    ASSERT_EQ(node_b.GetLiveSyncVersion(), 3);

    // Trigger Node A remote check
    ASSERT_TRUE(node_a.SyncTick());
    ASSERT_EQ(node_a.GetLiveSyncVersion(), 3);

    // Verify file1 updated on Node A
    std::string read_a_f1;
    {
        std::ifstream ifs(dir_a / "file1.txt");
        read_a_f1.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    }
    ASSERT_EQ(read_a_f1, updated_f1);

    // Verify sub/file2 deleted from Node A
    ASSERT_FALSE(std::filesystem::exists(dir_a / "sub" / "file2.txt"));

    node_a.Stop();
    node_b.Stop();
    std::filesystem::remove_all(test_root);
}
