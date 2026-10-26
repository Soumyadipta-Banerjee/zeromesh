#pragma once

#include "tests/examples/torrent_manifest.hpp"
#include "cas/store.hpp"
#include "dht/kademlia.hpp"
#include "discovery/mdns.hpp"
#include "discovery/peer_table.hpp"
#include "vfs/chunk_fetcher.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace zeromesh::examples {

inline dht::Key HexToKey(const std::string& hex) {
    dht::Key k{};
    for (size_t i = 0; i < k.size() && (i * 2 + 1) < hex.size(); ++i) {
        auto byte_str = hex.substr(i * 2, 2);
        k[i] = static_cast<uint8_t>(std::strtoul(byte_str.c_str(), nullptr, 16));
    }
    return k;
}

inline std::string KeyToHex(const dht::Key& key) {
    std::ostringstream oss;
    for (uint8_t b : key) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

/**
 * @brief Statistics of the active torrent swarm.
 */
struct SwarmStats {
    uint32_t active_peers{0};
    uint64_t total_chunks_local{0};
    uint64_t total_bytes_seeded{0};
    uint64_t total_bytes_downloaded{0};
    uint16_t dht_port{8982};
    uint16_t chunk_port{8984};
    std::string self_node_id;
};

/**
 * @brief TorrentEngine coordinates directory chunking, seeding, DHT announces,
 * peer discovery, and BitTorrent-style piece downloading.
 */
class TorrentEngine {
public:
    using ProgressCallback = std::function<void(float fraction,
                                                const std::string& status,
                                                uint64_t bytes_transferred,
                                                uint64_t total_bytes)>;

    TorrentEngine(std::filesystem::path storage_dir,
                  uint16_t dht_port = 8982,
                  uint16_t chunk_port = 8984)
        : storage_dir_(std::move(storage_dir)),
          dht_port_(dht_port),
          chunk_port_(chunk_port) {
        
        std::filesystem::create_directories(storage_dir_ / "cas");
        std::filesystem::create_directories(storage_dir_ / "cache");

        cas_store_ = std::make_shared<cas::CasStore>(storage_dir_ / "cas");
        chunk_cache_ = std::make_shared<vfs::ChunkCache>(storage_dir_ / "cache", 1024ULL * 1024 * 1024);

        // Generate a random unique 256-bit NodeId
        dht::NodeId self_id{};
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        for (size_t i = 0; i < self_id.size(); ++i) {
            self_id[i] = static_cast<uint8_t>((now >> (i % 8 * 8)) ^ (i * 37 + dht_port));
        }
        self_node_id_hex_ = KeyToHex(self_id);

        dht_ = std::make_shared<dht::KademliaDht>(self_id, dht_port_);

        vfs::ChunkFetcherConfig fetcher_cfg;
        fetcher_cfg.local_serve_port = chunk_port_;
        fetcher_cfg.remote_chunk_port = chunk_port_;
        fetcher_cfg.fetch_timeout = std::chrono::milliseconds(4000);
        chunk_fetcher_ = std::make_shared<vfs::ChunkFetcher>(fetcher_cfg, dht_, cas_store_, chunk_cache_);

        peer_table_ = std::make_shared<discovery::PeerTable>();
        discovery::MdnsConfig mdns_cfg;
        mdns_cfg.node_id = self_node_id_hex_;
        mdns_cfg.grpc_port = dht_port_;
        mdns_ = std::make_shared<discovery::MdnsEngine>(peer_table_, mdns_cfg);
    }

    ~TorrentEngine() {
        Stop();
    }

    bool Start() {
        if (running_.load()) return true;
        running_.store(true);

        dht_->Start();
        chunk_fetcher_->Start();
        mdns_->Start();

        // Background thread to sync mDNS peers into DHT routing table
        sync_thread_ = std::thread([this]() {
            while (running_.load()) {
                auto peers = peer_table_->ListActive();
                for (const auto& p : peers) {
                    if (p.id == self_node_id_hex_) continue;
                    dht::DhtNode node;
                    node.id = HexToKey(p.id);
                    node.ip_address = p.ip_address;
                    node.port = dht_port_;
                    node.last_seen = std::chrono::steady_clock::now();
                    dht_->InsertNode(node);
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        });

        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;

        if (sync_thread_.joinable()) {
            sync_thread_.join();
        }

        mdns_->Stop();
        chunk_fetcher_->Stop();
        dht_->Stop();
    }

    void AddDhtPeer(const std::string& ip, uint16_t dht_port, const std::string& node_id_hex) {
        dht::DhtNode node;
        node.id = HexToKey(node_id_hex);
        node.ip_address = ip;
        node.port = dht_port;
        node.last_seen = std::chrono::steady_clock::now();
        dht_->InsertNode(node);
    }

    [[nodiscard]] std::shared_ptr<dht::KademliaDht> GetDht() const {
        return dht_;
    }

    [[nodiscard]] SwarmStats GetStats() const {
        SwarmStats s;
        s.active_peers = static_cast<uint32_t>(peer_table_->Count());
        s.dht_port = dht_port_;
        s.chunk_port = chunk_port_;
        s.self_node_id = self_node_id_hex_;
        s.total_bytes_seeded = total_bytes_seeded_.load();
        s.total_bytes_downloaded = total_bytes_downloaded_.load();
        return s;
    }

    /**
     * @brief Exports a directory as a torrent, chunks all files, announces to DHT,
     * and returns the resulting TorrentManifest and info-hash.
     */
    std::pair<TorrentManifest, std::string> ExportDirectory(const std::filesystem::path& dir_path,
                                                          size_t chunk_size_bytes,
                                                          ProgressCallback progress_cb = nullptr) {
        TorrentManifest manifest;
        manifest.name = dir_path.filename().empty() ? "shared_folder" : dir_path.filename().string();
        manifest.chunk_size_bytes = chunk_size_bytes;
        manifest.created_at_sec = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        // First pass: collect files and calculate total bytes
        std::vector<std::filesystem::path> all_files;
        uint64_t total_dir_bytes = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
            if (entry.is_regular_file()) {
                all_files.push_back(entry.path());
                total_dir_bytes += entry.file_size();
            }
        }

        manifest.total_bytes = total_dir_bytes;

        uint64_t processed_bytes = 0;
        std::vector<char> buffer(chunk_size_bytes);

        for (const auto& file_path : all_files) {
            auto rel_path = std::filesystem::relative(file_path, dir_path).string();
            TorrentFileInfo file_info;
            file_info.relative_path = rel_path;
            file_info.size_bytes = std::filesystem::file_size(file_path);

            std::ifstream ifs(file_path, std::ios::binary);
            if (!ifs) continue;

            while (ifs) {
                ifs.read(buffer.data(), buffer.size());
                std::streamsize bytes_read = ifs.gcount();
                if (bytes_read <= 0) break;

                std::string chunk_data(buffer.data(), bytes_read);
                auto digest = cas_store_->PutBlob(chunk_data);
                chunk_cache_->Put(digest.hash, chunk_data);

                // Announce chunk to DHT
                chunk_fetcher_->AnnounceLocalChunk(digest.hash);

                file_info.chunk_hashes.push_back(digest.hash);
                processed_bytes += bytes_read;
                total_bytes_seeded_ += bytes_read;

                if (progress_cb) {
                    float frac = total_dir_bytes > 0 ? static_cast<float>(processed_bytes) / total_dir_bytes : 1.0f;
                    progress_cb(frac, "Chunking & Seeding: " + rel_path, processed_bytes, total_dir_bytes);
                }
            }

            manifest.files.push_back(std::move(file_info));
        }

        // Serialize and publish manifest
        std::string manifest_json = manifest.Serialize();
        std::string info_hash = TorrentManifest::ComputeInfoHash(manifest_json);

        // Store manifest in CAS and announce in DHT
        cas_store_->PutBlob(manifest_json);
        chunk_cache_->Put(info_hash, manifest_json);
        dht_->StoreValue(HexToKey(info_hash), manifest_json);
        chunk_fetcher_->AnnounceLocalChunk(info_hash);

        if (progress_cb) {
            progress_cb(1.0f, "Seeding active for: " + manifest.name, total_dir_bytes, total_dir_bytes);
        }

        return {manifest, info_hash};
    }

    /**
     * @brief Downloads a directory given its info-hash, reconstructs all files,
     * and announces them to become a seeder.
     */
    bool DownloadTorrent(const std::string& info_hash,
                         const std::filesystem::path& dest_parent_dir,
                         ProgressCallback progress_cb = nullptr) {
        if (progress_cb) {
            progress_cb(0.02f, "Querying DHT for manifest: " + info_hash.substr(0, 16) + "...", 0, 1);
        }

        // Step 1: Retrieve manifest JSON
        std::string manifest_json;
        auto val = dht_->FindValue(HexToKey(info_hash));
        if (val) {
            manifest_json = *val;
        } else {
            // Attempt chunk fetch fallback from providers
            auto res = chunk_fetcher_->FetchChunk(info_hash);
            if (res.success) {
                manifest_json = res.data;
            }
        }

        if (manifest_json.empty()) {
            if (progress_cb) {
                progress_cb(0.0f, "Error: Manifest not found in DHT swarm", 0, 0);
            }
            return false;
        }

        auto manifest_opt = TorrentManifest::Deserialize(manifest_json);
        if (!manifest_opt) {
            if (progress_cb) {
                progress_cb(0.0f, "Error: Corrupt manifest JSON", 0, 0);
            }
            return false;
        }

        const auto& manifest = *manifest_opt;
        std::filesystem::path target_dir = dest_parent_dir / manifest.name;
        std::filesystem::create_directories(target_dir);

        uint64_t total_bytes = manifest.total_bytes;
        uint64_t downloaded_bytes = 0;

        // Step 2: Download files and chunks
        for (const auto& file : manifest.files) {
            auto out_path = target_dir / file.relative_path;
            std::filesystem::create_directories(out_path.parent_path());

            std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                if (progress_cb) {
                    progress_cb(0.0f, "Error: Failed to create " + file.relative_path, downloaded_bytes, total_bytes);
                }
                return false;
            }

            for (size_t ch_idx = 0; ch_idx < file.chunk_hashes.size(); ++ch_idx) {
                const auto& ch_hash = file.chunk_hashes[ch_idx];
                if (progress_cb) {
                    float frac = total_bytes > 0 ? static_cast<float>(downloaded_bytes) / total_bytes : 0.0f;
                    progress_cb(frac, "Downloading: " + file.relative_path + " [chunk " +
                                std::to_string(ch_idx + 1) + "/" + std::to_string(file.chunk_hashes.size()) + "]",
                                downloaded_bytes, total_bytes);
                }

                auto chunk_res = chunk_fetcher_->FetchChunk(ch_hash);
                if (!chunk_res.success) {
                    if (progress_cb) {
                        progress_cb(0.0f, "Error: Failed to fetch chunk " + ch_hash.substr(0, 12),
                                    downloaded_bytes, total_bytes);
                    }
                    return false;
                }

                ofs.write(chunk_res.data.data(), chunk_res.data.size());
                downloaded_bytes += chunk_res.data.size();
                total_bytes_downloaded_ += chunk_res.data.size();
            }
            ofs.close();
        }

        // Step 3: Re-announce all downloaded chunks to become a seeder in the mesh
        for (const auto& file : manifest.files) {
            for (const auto& ch_hash : file.chunk_hashes) {
                chunk_fetcher_->AnnounceLocalChunk(ch_hash);
            }
        }
        chunk_fetcher_->AnnounceLocalChunk(info_hash);

        if (progress_cb) {
            progress_cb(1.0f, "Download Complete! Now seeding: " + manifest.name, total_bytes, total_bytes);
        }

        return true;
    }

private:
    std::filesystem::path storage_dir_;
    uint16_t dht_port_{8982};
    uint16_t chunk_port_{8984};
    std::string self_node_id_hex_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> total_bytes_seeded_{0};
    std::atomic<uint64_t> total_bytes_downloaded_{0};

    std::shared_ptr<cas::CasStore> cas_store_;
    std::shared_ptr<vfs::ChunkCache> chunk_cache_;
    std::shared_ptr<dht::KademliaDht> dht_;
    std::shared_ptr<vfs::ChunkFetcher> chunk_fetcher_;
    std::shared_ptr<discovery::PeerTable> peer_table_;
    std::shared_ptr<discovery::MdnsEngine> mdns_;
    std::thread sync_thread_;
};

} // namespace zeromesh::examples
