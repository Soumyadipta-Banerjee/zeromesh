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
    bool download_ok = node_b.DownloadTorrent(info_hash, node_b_dest);
    ASSERT_TRUE(download_ok);

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
