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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace zeromesh::examples {

inline std::string GetLocalLanIp() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "127.0.0.1";

    struct sockaddr_in loopback{};
    loopback.sin_family = AF_INET;
    loopback.sin_port = htons(9); // Discard port
    inet_pton(AF_INET, "8.8.8.8", &loopback.sin_addr);

    // Connecting a UDP socket does NOT send any packets on the wire,
    // but causes the kernel routing table to bind the local interface IP.
    if (connect(sock, reinterpret_cast<struct sockaddr*>(&loopback), sizeof(loopback)) < 0) {
        close(sock);
        return "127.0.0.1";
    }

    struct sockaddr_in local_addr{};
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sock, reinterpret_cast<struct sockaddr*>(&local_addr), &addr_len) < 0) {
        close(sock);
        return "127.0.0.1";
    }

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local_addr.sin_addr, ip_str, sizeof(ip_str));
    close(sock);
    return std::string(ip_str);
}

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
 * @brief Result of a torrent download operation with detailed error diagnostics.
 */
struct DownloadResult {
    bool success{false};
    std::string error_message;
    uint64_t bytes_downloaded{0};
    uint64_t total_bytes{0};

    operator bool() const { return success; }
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

    using SyncEventCallback = std::function<void(const std::string& event_type,
                                                 const std::string& details,
                                                 uint64_t version,
                                                 size_t file_count)>;

    struct SyncFolderState {
        std::string swarm_id;
        std::filesystem::path local_dir;
        std::atomic<uint64_t> current_version{1};
        size_t chunk_size_bytes{256 * 1024};
        std::atomic<bool> active{false};
        std::atomic<bool> is_applying_remote{false};
        std::string latest_manifest_hash;

        struct FileSnapshot {
            uint64_t size{0};
            std::filesystem::file_time_type mtime{};
            std::vector<std::string> chunk_hashes;
        };
        std::unordered_map<std::string, FileSnapshot> file_snapshot;
        std::vector<std::pair<std::string, uint16_t>> known_dht_peers;
        std::vector<std::pair<std::string, uint16_t>> known_chunk_peers;
        std::recursive_mutex sync_mutex;
    };

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

                    std::lock_guard<std::recursive_mutex> lock(sync_state_.sync_mutex);
                    bool found = false;
                    for (const auto& kp : sync_state_.known_dht_peers) {
                        if (kp.first == p.ip_address && kp.second == dht_port_) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        sync_state_.known_dht_peers.push_back({p.ip_address, dht_port_});
                        sync_state_.known_chunk_peers.push_back({p.ip_address, chunk_port_});
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        });

        return true;
    }

    void Stop() {
        if (!running_.exchange(false)) return;

        StopLiveSync();
        if (live_sync_thread_.joinable()) {
            live_sync_thread_.join();
        }

        if (sync_thread_.joinable()) {
            sync_thread_.join();
        }

        mdns_->Stop();
        chunk_fetcher_->Stop();
        dht_->Stop();
    }

    void SetSyncCallback(SyncEventCallback cb) {
        sync_event_cb_ = std::move(cb);
    }

    [[nodiscard]] bool IsLiveSyncActive() const {
        return sync_state_.active.load();
    }

    [[nodiscard]] uint64_t GetLiveSyncVersion() const {
        return sync_state_.current_version.load();
    }

    [[nodiscard]] std::string GetLiveSyncSwarmId() const {
        return sync_state_.swarm_id;
    }

    void AddDhtPeer(const std::string& ip, uint16_t dht_port, const std::string& node_id_hex, uint16_t chunk_port = 0) {
        dht::DhtNode node;
        node.id = HexToKey(node_id_hex);
        node.ip_address = ip;
        node.port = dht_port;
        node.last_seen = std::chrono::steady_clock::now();
        dht_->InsertNode(node);

        std::lock_guard<std::recursive_mutex> lock(sync_state_.sync_mutex);
        sync_state_.known_dht_peers.push_back({ip, dht_port});
        sync_state_.known_chunk_peers.push_back({ip, chunk_port > 0 ? chunk_port : chunk_port_});
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

    void PublishSwarmHead(const std::string& swarm_id,
                          uint64_t version,
                          const std::string& manifest_hash,
                          const std::string& manifest_json) {
        SwarmHead head;
        head.swarm_id = swarm_id;
        head.version = version;
        head.manifest_hash = manifest_hash;
        head.updated_at_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        if (manifest_json.size() <= 7000) {
            head.manifest_json = manifest_json;
        }

        std::string head_json = head.Serialize();
        dht_->StoreValue(HexToKey(swarm_id), head_json);
        dht_->BroadcastStore(HexToKey(swarm_id), head_json);

        std::vector<std::pair<std::string, uint16_t>> peers_copy;
        {
            std::lock_guard<std::recursive_mutex> lock(sync_state_.sync_mutex);
            peers_copy = sync_state_.known_dht_peers;
        }
        for (const auto& peer : peers_copy) {
            dht_->StoreValueDirect(peer.first, peer.second, HexToKey(swarm_id), head_json);
        }
    }

    void StartLiveSync(const std::string& swarm_id,
                       const std::filesystem::path& dir_path,
                       size_t chunk_size_bytes = 256 * 1024,
                       const std::string& peer_ip = "",
                       uint16_t peer_dht_port = 0,
                       uint16_t peer_chunk_port = 0,
                       const TorrentManifest* initial_manifest = nullptr) {
        {
            std::lock_guard<std::recursive_mutex> lock(sync_state_.sync_mutex);
            sync_state_.swarm_id = swarm_id;
            sync_state_.local_dir = dir_path;
            sync_state_.chunk_size_bytes = chunk_size_bytes;

            if (!peer_ip.empty()) {
                if (peer_dht_port > 0) sync_state_.known_dht_peers.push_back({peer_ip, peer_dht_port});
                if (peer_chunk_port > 0) sync_state_.known_chunk_peers.push_back({peer_ip, peer_chunk_port});
            }

            sync_state_.file_snapshot.clear();
            if (initial_manifest != nullptr) {
                sync_state_.current_version.store(initial_manifest->version > 0 ? initial_manifest->version : 1);
                sync_state_.latest_manifest_hash = swarm_id;
                for (const auto& fi : initial_manifest->files) {
                    auto full_p = dir_path / fi.relative_path;
                    std::error_code ec;
                    auto mtime = std::filesystem::last_write_time(full_p, ec);
                    sync_state_.file_snapshot[fi.relative_path] = {fi.size_bytes, mtime, fi.chunk_hashes};
                }
            } else {
                sync_state_.current_version.store(1);
                try {
                    if (std::filesystem::exists(dir_path)) {
                        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
                            if (entry.is_regular_file()) {
                                auto rel = std::filesystem::relative(entry.path(), dir_path).string();
                                sync_state_.file_snapshot[rel] = {entry.file_size(), entry.last_write_time(), {}};
                            }
                        }
                    }
                } catch (...) {}
            }

            sync_state_.active.store(true);
        }

        if (!live_sync_thread_.joinable()) {
            live_sync_thread_ = std::thread([this]() {
                while (running_.load()) {
                    if (sync_state_.active.load()) {
                        SyncTick();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
            });
        }
    }

    void StopLiveSync() {
        sync_state_.active.store(false);
    }

    bool CheckLocalChangesAndPublish() {
        std::unique_lock<std::recursive_mutex> lock(sync_state_.sync_mutex);
        if (!sync_state_.active.load() || sync_state_.is_applying_remote.load()) {
            return false;
        }
        if (!std::filesystem::exists(sync_state_.local_dir) || !std::filesystem::is_directory(sync_state_.local_dir)) {
            return false;
        }

        struct DiskFile {
            std::string rel_path;
            std::filesystem::path full_path;
            uint64_t size{0};
            std::filesystem::file_time_type mtime{};
        };
        std::vector<DiskFile> disk_files;
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(sync_state_.local_dir)) {
                if (entry.is_regular_file()) {
                    auto rel = std::filesystem::relative(entry.path(), sync_state_.local_dir).string();
                    disk_files.push_back({rel, entry.path(), entry.file_size(), entry.last_write_time()});
                }
            }
        } catch (...) {
            return false;
        }

        bool has_changes = false;
        std::unordered_set<std::string> on_disk_paths;
        for (const auto& df : disk_files) {
            on_disk_paths.insert(df.rel_path);
            auto it = sync_state_.file_snapshot.find(df.rel_path);
            if (it == sync_state_.file_snapshot.end()) {
                has_changes = true;
                break;
            } else if (it->second.size != df.size || it->second.mtime != df.mtime) {
                has_changes = true;
                break;
            }
        }
        if (!has_changes) {
            for (const auto& [snap_path, _] : sync_state_.file_snapshot) {
                if (on_disk_paths.find(snap_path) == on_disk_paths.end()) {
                    has_changes = true;
                    break;
                }
            }
        }

        if (!has_changes) return false;

        TorrentManifest manifest;
        manifest.name = sync_state_.local_dir.filename().empty() ? "shared_folder" : sync_state_.local_dir.filename().string();
        manifest.swarm_id = sync_state_.swarm_id;
        manifest.version = ++sync_state_.current_version;
        manifest.chunk_size_bytes = sync_state_.chunk_size_bytes;
        manifest.created_at_sec = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        manifest.updated_at_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        uint64_t total_bytes = 0;
        std::unordered_map<std::string, SyncFolderState::FileSnapshot> new_snapshot;
        std::vector<char> buffer(sync_state_.chunk_size_bytes);

        for (const auto& df : disk_files) {
            total_bytes += df.size;
            TorrentFileInfo fi;
            fi.relative_path = df.rel_path;
            fi.size_bytes = df.size;

            auto it = sync_state_.file_snapshot.find(df.rel_path);
            if (it != sync_state_.file_snapshot.end() && it->second.size == df.size &&
                it->second.mtime == df.mtime && !it->second.chunk_hashes.empty()) {
                fi.chunk_hashes = it->second.chunk_hashes;
                new_snapshot[df.rel_path] = it->second;
            } else {
                std::ifstream ifs(df.full_path, std::ios::binary);
                if (!ifs) continue;
                while (ifs) {
                    ifs.read(buffer.data(), buffer.size());
                    std::streamsize bytes_read = ifs.gcount();
                    if (bytes_read <= 0) break;

                    std::string chunk_data(buffer.data(), bytes_read);
                    auto digest = cas_store_->PutBlob(chunk_data);
                    chunk_cache_->Put(digest.hash, chunk_data);
                    chunk_fetcher_->AnnounceLocalChunk(digest.hash);
                    fi.chunk_hashes.push_back(digest.hash);
                    total_bytes_seeded_ += bytes_read;
                }
                new_snapshot[df.rel_path] = {df.size, df.mtime, fi.chunk_hashes};
            }
            manifest.files.push_back(std::move(fi));
        }
        manifest.total_bytes = total_bytes;
        sync_state_.file_snapshot = std::move(new_snapshot);

        std::string manifest_json = manifest.Serialize();
        std::string manifest_hash = TorrentManifest::ComputeInfoHash(manifest_json);
        cas_store_->PutBlob(manifest_json);
        chunk_cache_->Put(manifest_hash, manifest_json);
        chunk_fetcher_->AnnounceLocalChunk(manifest_hash);
        dht_->StoreValue(HexToKey(manifest_hash), manifest_json);

        PublishSwarmHead(sync_state_.swarm_id, manifest.version, manifest_hash, manifest_json);
        sync_state_.latest_manifest_hash = manifest_hash;

        if (sync_event_cb_) {
            sync_event_cb_("LOCAL_UPDATE", "Published swarm update v" + std::to_string(manifest.version) +
                                           " (" + std::to_string(manifest.files.size()) + " files)",
                           manifest.version, manifest.files.size());
        }
        return true;
    }

    bool CheckRemoteUpdatesAndApply() {
        std::unique_lock<std::recursive_mutex> lock(sync_state_.sync_mutex);
        if (!sync_state_.active.load() || sync_state_.is_applying_remote.load()) {
            return false;
        }

        auto val = dht_->FindValue(HexToKey(sync_state_.swarm_id));
        if (!val || val->empty()) {
            return false;
        }

        std::optional<TorrentManifest> remote_manifest;
        auto head_opt = SwarmHead::Deserialize(*val);
        if (head_opt) {
            if (head_opt->version <= sync_state_.current_version.load()) {
                return false;
            }
            if (!head_opt->manifest_json.empty()) {
                remote_manifest = TorrentManifest::Deserialize(head_opt->manifest_json);
            } else if (!head_opt->manifest_hash.empty()) {
                auto m_res = chunk_fetcher_->FetchChunk(head_opt->manifest_hash);
                if (!m_res.success) {
                    for (const auto& p : sync_state_.known_chunk_peers) {
                        m_res = chunk_fetcher_->FetchFromPeerDirect(p.first, p.second, head_opt->manifest_hash);
                        if (m_res.success) break;
                    }
                }
                if (m_res.success) {
                    remote_manifest = TorrentManifest::Deserialize(m_res.data);
                }
            }
        } else {
            auto m_opt = TorrentManifest::Deserialize(*val);
            if (m_opt && m_opt->version > sync_state_.current_version.load()) {
                remote_manifest = m_opt;
            }
        }

        if (!remote_manifest) return false;

        sync_state_.is_applying_remote.store(true);

        if (sync_event_cb_) {
            sync_event_cb_("REMOTE_DETECTED", "Discovered swarm update v" + std::to_string(remote_manifest->version) +
                                              " (" + std::to_string(remote_manifest->files.size()) + " files). Synchronizing...",
                           remote_manifest->version, remote_manifest->files.size());
        }

        std::unordered_set<std::string> remote_paths;
        for (const auto& f : remote_manifest->files) {
            remote_paths.insert(f.relative_path);
        }

        size_t deleted_count = 0;
        for (auto it = sync_state_.file_snapshot.begin(); it != sync_state_.file_snapshot.end();) {
            if (remote_paths.find(it->first) == remote_paths.end()) {
                auto path_to_remove = sync_state_.local_dir / it->first;
                std::error_code ec;
                std::filesystem::remove(path_to_remove, ec);
                auto parent = path_to_remove.parent_path();
                if (parent != sync_state_.local_dir && std::filesystem::is_empty(parent, ec)) {
                    std::filesystem::remove(parent, ec);
                }
                deleted_count++;
                it = sync_state_.file_snapshot.erase(it);
            } else {
                ++it;
            }
        }

        size_t updated_count = 0;
        for (const auto& file : remote_manifest->files) {
            auto out_path = sync_state_.local_dir / file.relative_path;
            auto snap_it = sync_state_.file_snapshot.find(file.relative_path);

            bool identical = false;
            if (snap_it != sync_state_.file_snapshot.end() && std::filesystem::exists(out_path)) {
                if (snap_it->second.chunk_hashes == file.chunk_hashes && snap_it->second.size == file.size_bytes) {
                    identical = true;
                }
            }

            if (identical) continue;

            try {
                std::filesystem::create_directories(out_path.parent_path());
            } catch (...) {}

            std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
            if (!ofs) continue;

            bool file_ok = true;
            for (const auto& ch_hash : file.chunk_hashes) {
                auto chunk_res = chunk_fetcher_->FetchChunk(ch_hash);
                if (!chunk_res.success) {
                    for (const auto& p : sync_state_.known_chunk_peers) {
                        chunk_res = chunk_fetcher_->FetchFromPeerDirect(p.first, p.second, ch_hash);
                        if (chunk_res.success) break;
                    }
                }
                if (!chunk_res.success) {
                    file_ok = false;
                    break;
                }
                ofs.write(chunk_res.data.data(), chunk_res.data.size());
                chunk_fetcher_->AnnounceLocalChunk(ch_hash);
            }
            ofs.close();

            if (file_ok) {
                updated_count++;
                std::error_code ec;
                auto mtime = std::filesystem::last_write_time(out_path, ec);
                sync_state_.file_snapshot[file.relative_path] = {file.size_bytes, mtime, file.chunk_hashes};
            }
        }

        sync_state_.current_version.store(remote_manifest->version);
        sync_state_.latest_manifest_hash = TorrentManifest::ComputeInfoHash(remote_manifest->Serialize());
        chunk_fetcher_->AnnounceLocalChunk(sync_state_.latest_manifest_hash);

        sync_state_.is_applying_remote.store(false);

        if (sync_event_cb_) {
            std::string desc = "Swarm synchronized to v" + std::to_string(sync_state_.current_version.load());
            if (updated_count > 0 || deleted_count > 0) {
                desc += " (" + std::to_string(updated_count) + " updated, " + std::to_string(deleted_count) + " deleted)";
            }
            sync_event_cb_("SYNC_COMPLETE", desc, sync_state_.current_version.load(), remote_manifest->files.size());
        }
        return true;
    }

    bool SyncTick() {
        bool local = CheckLocalChangesAndPublish();
        bool remote = CheckRemoteUpdatesAndApply();
        return local || remote;
    }


    /**
     * @brief Exports a directory as a torrent, chunks all files, announces to DHT,
     * and returns the resulting TorrentManifest and info-hash.
     */
    std::pair<TorrentManifest, std::string> ExportDirectory(const std::filesystem::path& dir_path,
                                                          size_t chunk_size_bytes,
                                                          ProgressCallback progress_cb = nullptr) {
        if (!std::filesystem::exists(dir_path)) {
            throw std::runtime_error("Directory does not exist: " + dir_path.string());
        }
        if (!std::filesystem::is_directory(dir_path)) {
            throw std::runtime_error("Path is not a directory: " + dir_path.string());
        }

        TorrentManifest manifest;
        manifest.name = dir_path.filename().empty() ? "shared_folder" : dir_path.filename().string();
        manifest.chunk_size_bytes = chunk_size_bytes;
        manifest.created_at_sec = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        // First pass: collect files and calculate total bytes
        std::vector<std::filesystem::path> all_files;
        uint64_t total_dir_bytes = 0;
        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dir_path)) {
                if (entry.is_regular_file()) {
                    all_files.push_back(entry.path());
                    total_dir_bytes += entry.file_size();
                }
            }
        } catch (const std::exception& ex) {
            throw std::runtime_error(std::string("Filesystem read error scanning ") + dir_path.string() + ": " + ex.what());
        }

        if (all_files.empty()) {
            throw std::runtime_error("Directory contains no regular files to share: " + dir_path.string());
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
            if (!ifs) {
                throw std::runtime_error("Failed to open file for reading: " + file_path.string());
            }

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
        manifest.swarm_id = info_hash;
        manifest.version = 1;
        manifest_json = manifest.Serialize();

        // Store manifest in CAS and announce in DHT
        cas_store_->PutBlob(manifest_json);
        chunk_cache_->Put(info_hash, manifest_json);
        dht_->StoreValue(HexToKey(info_hash), manifest_json);
        chunk_fetcher_->AnnounceLocalChunk(info_hash);

        // Publish SwarmHead and start live sync
        PublishSwarmHead(info_hash, 1, info_hash, manifest_json);
        StartLiveSync(info_hash, dir_path, chunk_size_bytes, "", 0, 0, &manifest);

        if (progress_cb) {
            progress_cb(1.0f, "Seeding & Live Sync active for: " + manifest.name, total_dir_bytes, total_dir_bytes);
        }

        return {manifest, info_hash};
    }

    /**
     * @brief Downloads a directory given its info-hash, reconstructs all files,
     * and announces them to become a seeder.
     * Optionally accepts a bootstrap peer IP, DHT port, and chunk port.
     */
    DownloadResult DownloadTorrent(const std::string& info_hash,
                                  const std::filesystem::path& dest_parent_dir,
                                  ProgressCallback progress_cb = nullptr,
                                  const std::string& bootstrap_peer_ip = "",
                                  uint16_t bootstrap_dht_port = 0,
                                  uint16_t bootstrap_chunk_port = 0) {
        if (progress_cb) {
            progress_cb(0.02f, "Connecting to swarm & resolving manifest: " + info_hash.substr(0, 16) + "...", 0, 1);
        }

        // If a bootstrap peer is specified in the magnet link, register it into our DHT immediately
        if (!bootstrap_peer_ip.empty() && bootstrap_dht_port > 0) {
            dht::DhtNode node;
            // Generate synthetic deterministic node ID from IP & port if unknown
            auto synthetic_key = HexToKey(info_hash);
            synthetic_key[0] ^= 0xAA;
            node.id = synthetic_key;
            node.ip_address = bootstrap_peer_ip;
            node.port = bootstrap_dht_port;
            node.last_seen = std::chrono::steady_clock::now();
            dht_->InsertNode(node);
        }

        // Step 1: Retrieve manifest JSON with retry loop
        std::string manifest_json;
        constexpr int kMaxManifestRetries = 10;
        for (int attempt = 1; attempt <= kMaxManifestRetries; ++attempt) {
            // First check DHT key-value store
            auto val = dht_->FindValue(HexToKey(info_hash));
            if (val) {
                manifest_json = *val;
                break;
            }

            // Fallback: chunk fetch from DHT providers
            auto res = chunk_fetcher_->FetchChunk(info_hash);
            if (res.success) {
                manifest_json = res.data;
                break;
            }

            // Fallback: direct TCP fetch from bootstrap peer if known
            if (!bootstrap_peer_ip.empty() && bootstrap_chunk_port > 0) {
                auto direct_res = chunk_fetcher_->FetchFromPeerDirect(bootstrap_peer_ip, bootstrap_chunk_port, info_hash);
                if (direct_res.success) {
                    manifest_json = direct_res.data;
                    break;
                }
            }

            if (attempt < kMaxManifestRetries) {
                if (progress_cb) {
                    progress_cb(0.02f + 0.01f * attempt,
                                "Locating seeders in swarm (attempt " + std::to_string(attempt) + "/" +
                                std::to_string(kMaxManifestRetries) + ")...", 0, 1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }

        if (manifest_json.empty()) {
            std::string err = "Manifest " + info_hash.substr(0, 16) + "... not found in DHT swarm (no seeders reachable)";
            if (progress_cb) {
                progress_cb(0.0f, "Error: " + err, 0, 0);
            }
            return {false, err, 0, 0};
        }

        auto manifest_opt = TorrentManifest::Deserialize(manifest_json);
        if (!manifest_opt) {
            auto head_opt = SwarmHead::Deserialize(manifest_json);
            if (head_opt) {
                if (!head_opt->manifest_json.empty()) {
                    manifest_opt = TorrentManifest::Deserialize(head_opt->manifest_json);
                } else if (!head_opt->manifest_hash.empty()) {
                    auto mres = chunk_fetcher_->FetchChunk(head_opt->manifest_hash);
                    if (!mres.success && !bootstrap_peer_ip.empty() && bootstrap_chunk_port > 0) {
                        mres = chunk_fetcher_->FetchFromPeerDirect(bootstrap_peer_ip, bootstrap_chunk_port, head_opt->manifest_hash);
                    }
                    if (mres.success) {
                        manifest_opt = TorrentManifest::Deserialize(mres.data);
                    }
                }
            }
        }

        if (!manifest_opt) {
            std::string err = "Corrupted or malformed manifest JSON for hash " + info_hash;
            if (progress_cb) {
                progress_cb(0.0f, "Error: " + err, 0, 0);
            }
            return {false, err, 0, 0};
        }

        const auto& manifest = *manifest_opt;
        std::filesystem::path target_dir = dest_parent_dir / manifest.name;

        try {
            std::filesystem::create_directories(target_dir);
        } catch (const std::exception& ex) {
            std::string err = "Failed to create destination directory " + target_dir.string() + ": " + ex.what();
            if (progress_cb) progress_cb(0.0f, "Error: " + err, 0, 0);
            return {false, err, 0, 0};
        }

        // Check available disk space
        try {
            auto space_info = std::filesystem::space(dest_parent_dir);
            if (space_info.available < manifest.total_bytes) {
                std::string err = "Insufficient disk space: need " + std::to_string(manifest.total_bytes / 1024) +
                                  " KB, but only " + std::to_string(space_info.available / 1024) + " KB available";
                if (progress_cb) progress_cb(0.0f, "Error: " + err, 0, 0);
                return {false, err, 0, 0};
            }
        } catch (...) {}

        uint64_t total_bytes = manifest.total_bytes;
        uint64_t downloaded_bytes = 0;

        // Step 2: Download files and chunks
        for (const auto& file : manifest.files) {
            auto out_path = target_dir / file.relative_path;
            try {
                std::filesystem::create_directories(out_path.parent_path());
            } catch (const std::exception& ex) {
                std::string err = "Failed to create directory " + out_path.parent_path().string() + ": " + ex.what();
                if (progress_cb) progress_cb(0.0f, "Error: " + err, downloaded_bytes, total_bytes);
                return {false, err, downloaded_bytes, total_bytes};
            }

            std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                std::string err = "Failed to open output file: " + out_path.string();
                if (progress_cb) {
                    progress_cb(0.0f, "Error: " + err, downloaded_bytes, total_bytes);
                }
                return {false, err, downloaded_bytes, total_bytes};
            }

            for (size_t ch_idx = 0; ch_idx < file.chunk_hashes.size(); ++ch_idx) {
                const auto& ch_hash = file.chunk_hashes[ch_idx];
                if (progress_cb) {
                    float frac = total_bytes > 0 ? static_cast<float>(downloaded_bytes) / total_bytes : 0.0f;
                    progress_cb(frac, "Downloading: " + file.relative_path + " [chunk " +
                                std::to_string(ch_idx + 1) + "/" + std::to_string(file.chunk_hashes.size()) + "]",
                                downloaded_bytes, total_bytes);
                }

                vfs::ChunkResult chunk_res = chunk_fetcher_->FetchChunk(ch_hash);
                // If DHT provider lookup didn't find it yet, try direct fetch from bootstrap peer
                if (!chunk_res.success && !bootstrap_peer_ip.empty() && bootstrap_chunk_port > 0) {
                    chunk_res = chunk_fetcher_->FetchFromPeerDirect(bootstrap_peer_ip, bootstrap_chunk_port, ch_hash);
                }

                if (!chunk_res.success) {
                    std::string err = "Chunk " + ch_hash.substr(0, 16) + "... for file '" + file.relative_path + "' failed to fetch from any DHT provider";
                    if (progress_cb) {
                        progress_cb(0.0f, "Error: " + err, downloaded_bytes, total_bytes);
                    }
                    return {false, err, downloaded_bytes, total_bytes};
                }

                ofs.write(chunk_res.data.data(), chunk_res.data.size());
                if (ofs.bad() || ofs.fail()) {
                    std::string err = "Disk write failure while writing chunk to " + out_path.string();
                    if (progress_cb) progress_cb(0.0f, "Error: " + err, downloaded_bytes, total_bytes);
                    return {false, err, downloaded_bytes, total_bytes};
                }

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

        std::string effective_swarm_id = manifest.swarm_id.empty() ? info_hash : manifest.swarm_id;
        StartLiveSync(effective_swarm_id, target_dir, manifest.chunk_size_bytes,
                      bootstrap_peer_ip, bootstrap_dht_port, bootstrap_chunk_port, &manifest);

        if (progress_cb) {
            progress_cb(1.0f, "Download Complete! Now seeding & live syncing: " + manifest.name, total_bytes, total_bytes);
        }

        return {true, "", downloaded_bytes, total_bytes};
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

    // Live Swarm Sync Subsystem
    SyncFolderState sync_state_;
    std::thread live_sync_thread_;
    SyncEventCallback sync_event_cb_;
};

} // namespace zeromesh::examples

