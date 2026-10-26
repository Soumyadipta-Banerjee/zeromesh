#include "torrent_engine.hpp"
#include "torrent_manifest.hpp"

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_running{true};

void HandleSigInt(int) {
    g_running.store(false);
}

void PrintUsage(const char* prog_name) {
    std::cout << "\033[1;36mZeroMesh P2P Two-Way Live Directory Sync (CLI)\033[0m\n"
              << "Usage:\n"
              << "  " << prog_name << " export <directory_path> [dht_port] [chunk_port]\n"
              << "  " << prog_name << " sync   <magnet_uri_or_hash> <destination_dir> [peer_ip] [peer_dht_port] [peer_chunk_port] [my_dht_port] [my_chunk_port]\n\n"
              << "Examples:\n"
              << "  Node A (Terminal 1):\n"
              << "    " << prog_name << " export ./my_folder 8982 8984\n\n"
              << "  Node B (Terminal 2):\n"
              << "    " << prog_name << " sync \"magnet:?xt=urn:btih:...\" ./downloads 127.0.0.1 8982 8984 8992 8994\n\n"
              << "Both nodes will continuously synchronize creations, edits, and deletions bidirectionally.\n";
}
} // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        PrintUsage(argv[0]);
        return 1;
    }

    std::signal(SIGINT, HandleSigInt);
    std::signal(SIGTERM, HandleSigInt);

    std::string mode = argv[1];

    if (mode == "export") {
        std::filesystem::path dir_path = argv[2];
        if (!std::filesystem::exists(dir_path) || !std::filesystem::is_directory(dir_path)) {
            std::cerr << "\033[1;31m[ERROR] Path does not exist or is not a directory: " << dir_path << "\033[0m\n";
            return 1;
        }

        uint16_t dht_port = (argc >= 4) ? static_cast<uint16_t>(std::stoi(argv[3])) : 8982;
        uint16_t chunk_port = (argc >= 5) ? static_cast<uint16_t>(std::stoi(argv[4])) : 8984;

        auto storage_dir = std::filesystem::temp_directory_path() / ("zm_cli_" + std::to_string(dht_port));
        auto engine = std::make_unique<zeromesh::examples::TorrentEngine>(storage_dir, dht_port, chunk_port);
        if (!engine->Start()) {
            std::cerr << "\033[1;31m[ERROR] Failed to start ZeroMesh Torrent Engine on ports " << dht_port << "/" << chunk_port << "\033[0m\n";
            return 1;
        }

        engine->SetSyncCallback([](const std::string& type, const std::string& desc, uint64_t version, size_t file_count) {
            std::cout << "\033[1;35m[LIVE-SYNC]\033[0m \033[1;32m(" << type << " v" << version << ")\033[0m "
                      << desc << " [" << file_count << " files]" << std::endl;
        });

        std::cout << "\033[1;32m[OK]\033[0m Engine started. DHT port: " << dht_port << " | Chunk port: " << chunk_port << "\n";
        std::cout << "\033[1;33m[INFO]\033[0m Scanning and chunking directory: " << dir_path << " ...\n";

        auto [manifest, info_hash] = engine->ExportDirectory(
            dir_path, 256 * 1024,
            [](float fraction, const std::string& status, uint64_t transferred, uint64_t total) {
                std::cout << "\r\033[K[EXPORT " << static_cast<int>(fraction * 100) << "%] "
                          << status << " (" << (transferred / 1024) << " / " << (total / 1024) << " KB)" << std::flush;
            });

        std::cout << "\n\n\033[1;32m✓ Directory successfully exported and seeded!\033[0m\n";
        std::cout << "\033[1mInfoHash:   \033[0m" << info_hash << "\n";

        std::string magnet = zeromesh::examples::TorrentManifest::MakeMagnetUri(
            info_hash, manifest.name, manifest.total_bytes, "127.0.0.1", dht_port, chunk_port);
        std::cout << "\033[1mMagnet URI: \033[0m\033[36m" << magnet << "\033[0m\n\n";
        std::cout << "\033[1;37mWatching for changes... (Press Ctrl+C to stop)\033[0m\n";

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        std::cout << "\n\033[1;33mShutting down engine...\033[0m\n";
        engine->Stop();
        return 0;

    } else if (mode == "sync") {
        std::string uri_or_hash = argv[2];
        std::filesystem::path dest_dir = argv[3];

        auto parsed_magnet = zeromesh::examples::TorrentManifest::ExtractMagnetMetadata(uri_or_hash);
        std::string info_hash = parsed_magnet ? parsed_magnet->info_hash : uri_or_hash;

        std::string peer_ip = parsed_magnet && !parsed_magnet->peer_ip.empty() ? parsed_magnet->peer_ip : "";
        uint16_t peer_dht_port = parsed_magnet ? parsed_magnet->dht_port : 0;
        uint16_t peer_chunk_port = parsed_magnet ? parsed_magnet->chunk_port : 0;

        if (argc >= 5) peer_ip = argv[4];
        if (argc >= 6) peer_dht_port = static_cast<uint16_t>(std::stoi(argv[5]));
        if (argc >= 7) peer_chunk_port = static_cast<uint16_t>(std::stoi(argv[6]));

        uint16_t my_dht_port = (argc >= 8) ? static_cast<uint16_t>(std::stoi(argv[7])) : 8992;
        uint16_t my_chunk_port = (argc >= 9) ? static_cast<uint16_t>(std::stoi(argv[8])) : 8994;

        auto storage_dir = std::filesystem::temp_directory_path() / ("zm_cli_" + std::to_string(my_dht_port));
        auto engine = std::make_unique<zeromesh::examples::TorrentEngine>(storage_dir, my_dht_port, my_chunk_port);
        if (!engine->Start()) {
            std::cerr << "\033[1;31m[ERROR] Failed to start engine on ports " << my_dht_port << "/" << my_chunk_port << "\033[0m\n";
            return 1;
        }

        engine->SetSyncCallback([](const std::string& type, const std::string& desc, uint64_t version, size_t file_count) {
            std::cout << "\033[1;35m[LIVE-SYNC]\033[0m \033[1;32m(" << type << " v" << version << ")\033[0m "
                      << desc << " [" << file_count << " files]" << std::endl;
        });

        std::cout << "\033[1;32m[OK]\033[0m Engine started. DHT port: " << my_dht_port << " | Chunk port: " << my_chunk_port << "\n";
        std::cout << "\033[1;33m[INFO]\033[0m Resolving and downloading swarm " << info_hash.substr(0, 16) << "... to " << dest_dir << "\n";

        auto res = engine->DownloadTorrent(
            info_hash, dest_dir,
            [](float fraction, const std::string& status, uint64_t transferred, uint64_t total) {
                std::cout << "\r\033[K[SYNC " << static_cast<int>(fraction * 100) << "%] "
                          << status << " (" << (transferred / 1024) << " / " << (total > 0 ? (total / 1024) : 0) << " KB)" << std::flush;
            },
            peer_ip, peer_dht_port, peer_chunk_port);

        if (!res.success) {
            std::cout << "\n\033[1;31m✗ Download failed: " << res.error_message << "\033[0m\n";
            return 1;
        }

        std::cout << "\n\n\033[1;32m✓ Download completed! Bidirectional live sync is ACTIVE.\033[0m\n";
        std::cout << "\033[1;37mAny files you create, edit, or delete in this folder will sync back to the seeder, and vice versa.\033[0m\n";
        std::cout << "\033[1;37m(Press Ctrl+C to stop)\033[0m\n";

        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        std::cout << "\n\033[1;33mShutting down engine...\033[0m\n";
        engine->Stop();
        return 0;
    } else {
        PrintUsage(argv[0]);
        return 1;
    }
}
