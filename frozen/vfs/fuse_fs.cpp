#include "vfs/fuse_fs.hpp"
#include "vfs/chunk_fetcher.hpp"
#include "cas/store.hpp"
#include "dht/kademlia.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace zeromesh::vfs {

// ─────────────────────────────────────────────────────────────────────────────
// Portable SHA-256 for chunk hashing during publish
// ─────────────────────────────────────────────────────────────────────────────

namespace {

class Sha256Engine {
public:
    Sha256Engine() { Reset(); }

    void Reset() {
        state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f; state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
        count_ = 0;
        buffer_len_ = 0;
    }

    void Update(const uint8_t* data, size_t len) {
        count_ += len;
        for (size_t i = 0; i < len; ++i) {
            buffer_[buffer_len_++] = data[i];
            if (buffer_len_ == 64) {
                Transform(buffer_.data());
                buffer_len_ = 0;
            }
        }
    }

    std::string Final() {
        uint64_t total_bits = count_ * 8;
        buffer_[buffer_len_++] = 0x80;
        if (buffer_len_ > 56) {
            while (buffer_len_ < 64) buffer_[buffer_len_++] = 0;
            Transform(buffer_.data());
            buffer_len_ = 0;
        }
        while (buffer_len_ < 56) buffer_[buffer_len_++] = 0;
        for (int i = 7; i >= 0; --i) {
            buffer_[buffer_len_++] = static_cast<uint8_t>((total_bits >> (i * 8)) & 0xff);
        }
        Transform(buffer_.data());

        std::ostringstream oss;
        for (uint32_t val : state_) {
            oss << std::hex << std::setfill('0') << std::setw(8) << val;
        }
        return oss.str();
    }

private:
    static inline uint32_t RoTr(uint32_t x, uint32_t n) {
        return (x >> n) | (x << (32 - n));
    }

    void Transform(const uint8_t* block) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
            0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
            0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
            0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
            0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
            0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t W[64];
        for (int t = 0; t < 16; ++t) {
            W[t] = (static_cast<uint32_t>(block[t * 4]) << 24) |
                   (static_cast<uint32_t>(block[t * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[t * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[t * 4 + 3]));
        }
        for (int t = 16; t < 64; ++t) {
            uint32_t s0 = RoTr(W[t-15], 7) ^ RoTr(W[t-15], 18) ^ (W[t-15] >> 3);
            uint32_t s1 = RoTr(W[t-2], 17) ^ RoTr(W[t-2], 19) ^ (W[t-2] >> 10);
            W[t] = W[t-16] + s0 + W[t-7] + s1;
        }

        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

        for (int t = 0; t < 64; ++t) {
            uint32_t S1 = RoTr(e, 6) ^ RoTr(e, 11) ^ RoTr(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + S1 + ch + K[t] + W[t];
            uint32_t S0 = RoTr(a, 2) ^ RoTr(a, 13) ^ RoTr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = S0 + maj;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }

        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }

    std::array<uint32_t, 8> state_{};
    std::array<uint8_t, 64> buffer_{};
    size_t buffer_len_{0};
    uint64_t count_{0};
};

/// SHA-256 hex digest for in-memory data.
std::string Sha256Hex(const char* data, size_t len) {
    Sha256Engine ctx;
    ctx.Update(reinterpret_cast<const uint8_t*>(data), len);
    return ctx.Final();
}

/// SHA-256 hex string → DHT Key.
dht::Key HexToKey(const std::string& hex) {
    dht::Key key{};
    for (size_t i = 0; i < 32 && i * 2 + 1 < hex.size(); ++i) {
        auto hi = hex[i * 2];
        auto lo = hex[i * 2 + 1];
        auto hd = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        key[i] = static_cast<uint8_t>((hd(hi) << 4) | hd(lo));
    }
    return key;
}

/// Normalize a virtual path: ensure it starts with '/', collapse multiple slashes.
std::string NormalizePath(std::string_view path) {
    std::string result;
    result.reserve(path.size() + 1);
    if (path.empty() || path[0] != '/') result += '/';

    bool prev_slash = false;
    for (char c : path) {
        if (c == '/') {
            if (!prev_slash) {
                result += '/';
                prev_slash = true;
            }
        } else {
            result += c;
            prev_slash = false;
        }
    }

    // Remove trailing slash (except for root "/").
    if (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

/// Extract the parent directory from a normalized path.
std::string ParentPath(const std::string& path) {
    if (path == "/" || path.empty()) return "/";
    auto pos = path.rfind('/');
    if (pos == 0) return "/";
    if (pos == std::string::npos) return "/";
    return path.substr(0, pos);
}

/// Extract the filename component from a normalized path.
std::string FileName(const std::string& path) {
    if (path == "/") return "";
    auto pos = path.rfind('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

/// Get current time as seconds since epoch.
uint64_t NowSec() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

/// Resolve ~ in path to $HOME.
std::filesystem::path ExpandHome(const std::filesystem::path& p) {
    std::string s = p.string();
    if (s.size() >= 1 && s[0] == '~') {
        const char* home = std::getenv("HOME");
        if (home) {
            return std::filesystem::path(home) / s.substr(2);
        }
    }
    return p;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// InodeTable — Thread-safe virtual inode registry
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Thread-safe virtual inode table managing the in-memory filesystem tree.
 *
 * Maps:
 *   - inode_number → FileInode (metadata + chunk hashes)
 *   - path → inode_number
 *   - parent_inode → list of DirEntry children
 */
class InodeTable {
public:
    InodeTable() {
        // Create root directory inode.
        FileInode root{};
        root.inode_number = kRootInode;
        root.type = InodeType::Directory;
        root.mode = 0755;
        root.nlink = 2;
        root.mtime_sec = NowSec();
        root.ctime_sec = root.mtime_sec;
        root.atime_sec = root.mtime_sec;

        inodes_[kRootInode] = std::move(root);
        path_to_inode_["/"] = kRootInode;
    }

    static constexpr uint64_t kRootInode = 1;

    /// Look up an inode by path.
    std::optional<FileInode> LookupByPath(const std::string& path) const {
        std::shared_lock lock(mutex_);
        auto it = path_to_inode_.find(path);
        if (it == path_to_inode_.end()) return std::nullopt;
        auto inode_it = inodes_.find(it->second);
        if (inode_it == inodes_.end()) return std::nullopt;
        return inode_it->second;
    }

    /// Look up an inode by number.
    std::optional<FileInode> LookupByInode(uint64_t ino) const {
        std::shared_lock lock(mutex_);
        auto it = inodes_.find(ino);
        if (it == inodes_.end()) return std::nullopt;
        return it->second;
    }

    /// List children of a directory inode.
    std::vector<DirEntry> ListDirectory(uint64_t parent_ino) const {
        std::shared_lock lock(mutex_);
        auto it = children_.find(parent_ino);
        if (it == children_.end()) return {};
        return it->second;
    }

    /// List children by path.
    std::vector<DirEntry> ListDirectoryByPath(const std::string& path) const {
        std::shared_lock lock(mutex_);
        auto path_it = path_to_inode_.find(path);
        if (path_it == path_to_inode_.end()) return {};
        auto child_it = children_.find(path_it->second);
        if (child_it == children_.end()) return {};
        return child_it->second;
    }

    /**
     * @brief Ensures all intermediate directories exist for a given path.
     * Creates directory inodes as needed.
     * @return The inode number of the final directory.
     */
    uint64_t EnsureDirectoryPath(const std::string& path) {
        std::unique_lock lock(mutex_);
        return EnsureDirectoryPathLocked(path);
    }

    /**
     * @brief Creates or updates a file inode at the given virtual path.
     * @return The inode number of the created/updated file.
     */
    uint64_t CreateFile(const std::string& path, const FileInode& inode) {
        std::unique_lock lock(mutex_);

        std::string norm = NormalizePath(path);
        std::string parent = ParentPath(norm);
        std::string name = FileName(norm);

        // Ensure parent exists.
        uint64_t parent_ino = EnsureDirectoryPathLocked(parent);

        // Check if file already exists.
        auto existing_it = path_to_inode_.find(norm);
        if (existing_it != path_to_inode_.end()) {
            // Update existing inode in place.
            uint64_t ino = existing_it->second;
            auto& existing_inode = inodes_[ino];
            existing_inode.size_bytes = inode.size_bytes;
            existing_inode.chunk_hashes = inode.chunk_hashes;
            existing_inode.mtime_sec = NowSec();
            existing_inode.mode = inode.mode;
            return ino;
        }

        // Create new inode.
        uint64_t ino = next_inode_++;
        FileInode new_inode = inode;
        new_inode.inode_number = ino;
        if (new_inode.mtime_sec == 0) new_inode.mtime_sec = NowSec();
        if (new_inode.ctime_sec == 0) new_inode.ctime_sec = new_inode.mtime_sec;
        if (new_inode.atime_sec == 0) new_inode.atime_sec = new_inode.mtime_sec;

        inodes_[ino] = std::move(new_inode);
        path_to_inode_[norm] = ino;

        // Add to parent's children.
        DirEntry entry;
        entry.name = name;
        entry.inode_number = ino;
        entry.type = inode.type;
        children_[parent_ino].push_back(std::move(entry));

        // Update parent nlink.
        inodes_[parent_ino].nlink++;

        return ino;
    }

    /// Returns total inode count.
    size_t InodeCount() const {
        std::shared_lock lock(mutex_);
        return inodes_.size();
    }

private:
    uint64_t EnsureDirectoryPathLocked(const std::string& path) {
        std::string norm = NormalizePath(path);

        // Already exists?
        auto it = path_to_inode_.find(norm);
        if (it != path_to_inode_.end()) return it->second;

        // Recursively ensure parent first.
        std::string parent = ParentPath(norm);
        uint64_t parent_ino = kRootInode;
        if (parent != norm) {
            parent_ino = EnsureDirectoryPathLocked(parent);
        }

        // Create the directory inode.
        uint64_t ino = next_inode_++;
        FileInode dir{};
        dir.inode_number = ino;
        dir.type = InodeType::Directory;
        dir.mode = 0755;
        dir.nlink = 2;
        dir.mtime_sec = NowSec();
        dir.ctime_sec = dir.mtime_sec;
        dir.atime_sec = dir.mtime_sec;

        inodes_[ino] = std::move(dir);
        path_to_inode_[norm] = ino;

        // Add to parent's children.
        std::string name = FileName(norm);
        DirEntry entry;
        entry.name = name;
        entry.inode_number = ino;
        entry.type = InodeType::Directory;
        children_[parent_ino].push_back(std::move(entry));

        return ino;
    }

    mutable std::shared_mutex mutex_;
    std::unordered_map<uint64_t, FileInode> inodes_;
    std::unordered_map<std::string, uint64_t> path_to_inode_;
    std::unordered_map<uint64_t, std::vector<DirEntry>> children_;
    std::atomic<uint64_t> next_inode_{2}; // 1 is root.
};

// ─────────────────────────────────────────────────────────────────────────────
// FuseFileSystem::Impl — PIMPL internals
// ─────────────────────────────────────────────────────────────────────────────

struct FuseFileSystem::Impl {
    FuseConfig config;
    std::shared_ptr<dht::KademliaDht> dht;
    std::shared_ptr<cas::CasStore> cas_store;

    InodeTable inode_table;
    std::shared_ptr<ChunkCache> chunk_cache;
    std::unique_ptr<ChunkFetcher> chunk_fetcher;

    VfsStats stats;

    std::atomic<bool> mounted{false};
    std::thread fuse_thread;

    // ── Construction ────────────────────────────────────────────────────

    Impl(FuseConfig cfg,
         std::shared_ptr<dht::KademliaDht> dht_engine,
         std::shared_ptr<cas::CasStore> store)
        : config(std::move(cfg))
        , dht(std::move(dht_engine))
        , cas_store(std::move(store)) {

        auto cache_path = ExpandHome(config.cache_dir);
        chunk_cache = std::make_shared<ChunkCache>(cache_path, config.max_cache_bytes);

        ChunkFetcherConfig fetch_config;
        fetch_config.fetch_timeout = config.fetch_timeout;
        fetch_config.local_serve_port = config.chunk_serve_port;
        fetch_config.remote_chunk_port = config.chunk_serve_port;
        fetch_config.max_concurrent_fetches = config.prefetch_window * 2;

        chunk_fetcher = std::make_unique<ChunkFetcher>(
            std::move(fetch_config), dht, cas_store, chunk_cache);
    }

    // ── File Chunking ───────────────────────────────────────────────────

    /**
     * Splits a file into fixed-size chunks, stores each in CAS,
     * announces via DHT, and returns the ordered list of chunk hashes.
     */
    std::vector<std::string> ChunkAndPublish(const std::filesystem::path& file_path) {
        std::ifstream in(file_path, std::ios::binary);
        if (!in.is_open()) return {};

        std::vector<std::string> chunk_hashes;
        std::vector<char> buffer(config.chunk_size_bytes);

        while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()))
               || in.gcount() > 0) {
            auto bytes_read = static_cast<size_t>(in.gcount());

            // Compute hash for this chunk.
            std::string hash = Sha256Hex(buffer.data(), bytes_read);

            // Store in CAS.
            cas_store->PutBlob(std::string_view(buffer.data(), bytes_read));

            // Store in chunk cache for fast serving.
            chunk_cache->Put(hash, std::string(buffer.data(), bytes_read));

            // Announce to DHT.
            dht::Key key = HexToKey(hash);
            dht->AnnounceChunk(key);

            chunk_hashes.push_back(std::move(hash));
        }

        return chunk_hashes;
    }

    // ── Read Operation ──────────────────────────────────────────────────

    /**
     * Reads bytes from a file inode at the given offset.
     * Fetches chunks lazily via the ChunkFetcher on cache miss.
     */
    std::string ReadFileData(const FileInode& inode, uint64_t offset, size_t size) {
        if (offset >= inode.size_bytes) return {};
        size = std::min(size, static_cast<size_t>(inode.size_bytes - offset));

        std::string result;
        result.reserve(size);

        size_t chunk_size = config.chunk_size_bytes;
        size_t start_chunk = static_cast<size_t>(offset / chunk_size);
        size_t end_chunk = static_cast<size_t>((offset + size - 1) / chunk_size);

        // Prefetch upcoming chunks for sequential read-ahead.
        if (end_chunk + 1 < inode.chunk_hashes.size()) {
            size_t prefetch_end = std::min(
                end_chunk + config.prefetch_window + 1,
                inode.chunk_hashes.size());
            std::vector<std::string> prefetch_hashes;
            for (size_t i = end_chunk + 1; i < prefetch_end; ++i) {
                prefetch_hashes.push_back(inode.chunk_hashes[i]);
            }
            chunk_fetcher->PrefetchChunks(prefetch_hashes);
        }

        for (size_t ci = start_chunk; ci <= end_chunk && ci < inode.chunk_hashes.size(); ++ci) {
            const auto& chunk_hash = inode.chunk_hashes[ci];

            // Fetch the chunk (cache → CAS → DHT → TCP).
            auto fetch_result = chunk_fetcher->FetchChunk(chunk_hash);
            if (!fetch_result.success) {
                stats.chunk_fetch_failures.fetch_add(1);
                std::cerr << "[zm-vfs] failed to fetch chunk " << chunk_hash
                          << " for read at offset " << offset << "\n";
                break; // Return partial data.
            }

            if (fetch_result.source_peer_ip == "local-cache" ||
                fetch_result.source_peer_ip == "local-cas") {
                stats.chunks_served_local.fetch_add(1);
            } else {
                stats.chunks_fetched_remote.fetch_add(1);
            }

            // Calculate the slice of this chunk that falls in [offset, offset+size).
            size_t chunk_start_byte = ci * chunk_size;
            size_t chunk_end_byte = chunk_start_byte + fetch_result.data.size();

            size_t copy_from = 0;
            if (offset > chunk_start_byte) {
                copy_from = static_cast<size_t>(offset - chunk_start_byte);
            }

            size_t copy_to = fetch_result.data.size();
            if (offset + size < chunk_end_byte) {
                copy_to = static_cast<size_t>(offset + size - chunk_start_byte);
            }

            if (copy_from < copy_to && copy_to <= fetch_result.data.size()) {
                result.append(fetch_result.data, copy_from, copy_to - copy_from);
            }
        }

        stats.bytes_read.fetch_add(result.size());
        return result;
    }

    // ── Write Operation ─────────────────────────────────────────────────

    /**
     * Writes bytes to a file inode at the given offset.
     * For simplicity, this re-chunks affected blocks.
     */
    size_t WriteFileData(FileInode& inode, uint64_t offset,
                         const char* data, size_t size) {
        if (config.read_only) return 0;

        // Read the entire file into memory (for simplicity in the initial version).
        std::string file_content;
        if (inode.size_bytes > 0) {
            file_content = ReadFileData(inode, 0, static_cast<size_t>(inode.size_bytes));
        }

        // Expand file if write goes beyond current size.
        size_t required_size = static_cast<size_t>(offset) + size;
        if (required_size > file_content.size()) {
            file_content.resize(required_size, '\0');
        }

        // Apply the write.
        std::memcpy(file_content.data() + offset, data, size);

        // Re-chunk the entire file.
        inode.chunk_hashes.clear();
        size_t chunk_size = config.chunk_size_bytes;
        size_t pos = 0;

        while (pos < file_content.size()) {
            size_t this_chunk = std::min(chunk_size, file_content.size() - pos);
            std::string hash = Sha256Hex(file_content.data() + pos, this_chunk);

            cas_store->PutBlob(std::string_view(file_content.data() + pos, this_chunk));
            chunk_cache->Put(hash, std::string(file_content.data() + pos, this_chunk));

            dht::Key key = HexToKey(hash);
            dht->AnnounceChunk(key);

            inode.chunk_hashes.push_back(std::move(hash));
            pos += this_chunk;
        }

        inode.size_bytes = file_content.size();
        inode.mtime_sec = NowSec();

        stats.bytes_written.fetch_add(size);
        return size;
    }

    // ── Metadata Serialization ──────────────────────────────────────────

    /**
     * Serializes the inode table to the DHT for persistence across restarts.
     * The manifest is stored under a well-known DHT key derived from the
     * volume key, allowing any peer to reconstruct the filesystem tree.
     */
    void PublishManifestToDht() {
        // Serialize the inode table as a simple line-based manifest format:
        // INODE <ino> <type> <mode> <size> <mtime> <path>
        // CHUNK <ino> <chunk_hash>
        // END

        std::ostringstream ss;
        ss << "ZMFS_MANIFEST_V1\n";

        // We'd need access to the inode table internals, so we use the
        // public Lookup/ReadDir interface via a BFS traversal.
        struct TraversalEntry {
            std::string path;
        };
        std::vector<TraversalEntry> queue;
        queue.push_back({"/"});

        while (!queue.empty()) {
            auto current = queue.back();
            queue.pop_back();

            auto inode = inode_table.LookupByPath(current.path);
            if (!inode) continue;

            ss << "INODE " << inode->inode_number
               << " " << static_cast<int>(inode->type)
               << " " << inode->mode
               << " " << inode->size_bytes
               << " " << inode->mtime_sec
               << " " << current.path << "\n";

            for (const auto& hash : inode->chunk_hashes) {
                ss << "CHUNK " << inode->inode_number << " " << hash << "\n";
            }

            if (inode->type == InodeType::Directory) {
                auto entries = inode_table.ListDirectoryByPath(current.path);
                for (const auto& entry : entries) {
                    std::string child_path = (current.path == "/")
                        ? "/" + entry.name
                        : current.path + "/" + entry.name;
                    queue.push_back({child_path});
                }
            }
        }

        ss << "END\n";

        // Store under a well-known key derived from volume_key_hex.
        std::string manifest_key_str = "zmfs-manifest-" + config.volume_key_hex;
        std::string manifest_key_hash = Sha256Hex(
            manifest_key_str.data(), manifest_key_str.size());
        dht::Key dht_key = HexToKey(manifest_key_hash);

        std::string manifest_data = ss.str();
        dht->StoreValue(dht_key, manifest_data);
    }

    /**
     * Attempts to reconstruct the filesystem tree from a DHT-stored manifest.
     */
    bool LoadManifestFromDht() {
        std::string manifest_key_str = "zmfs-manifest-" + config.volume_key_hex;
        std::string manifest_key_hash = Sha256Hex(
            manifest_key_str.data(), manifest_key_str.size());
        dht::Key dht_key = HexToKey(manifest_key_hash);

        auto manifest_data = dht->FindValue(dht_key);
        if (!manifest_data) return false;

        // Parse the manifest.
        std::istringstream iss(*manifest_data);
        std::string line;

        // Check header.
        if (!std::getline(iss, line) || line != "ZMFS_MANIFEST_V1") {
            return false;
        }

        // Map from inode numbers in the manifest to paths, for CHUNK lines.
        std::unordered_map<uint64_t, std::string> ino_to_path;

        while (std::getline(iss, line)) {
            if (line == "END") break;

            if (line.substr(0, 6) == "INODE ") {
                // Parse: INODE <ino> <type> <mode> <size> <mtime> <path>
                std::istringstream ls(line.substr(6));
                uint64_t ino;
                int type_int;
                uint32_t mode;
                uint64_t size_bytes, mtime;
                std::string path;

                if (!(ls >> ino >> type_int >> mode >> size_bytes >> mtime >> path)) {
                    continue;
                }

                ino_to_path[ino] = path;

                if (path == "/") continue; // Root already exists.

                FileInode inode;
                inode.type = static_cast<InodeType>(type_int);
                inode.mode = mode;
                inode.size_bytes = size_bytes;
                inode.mtime_sec = mtime;

                if (inode.type == InodeType::Directory) {
                    inode_table.EnsureDirectoryPath(path);
                } else {
                    inode_table.CreateFile(path, inode);
                }
            }
            else if (line.substr(0, 6) == "CHUNK ") {
                // Parse: CHUNK <ino> <hash>
                std::istringstream ls(line.substr(6));
                uint64_t ino;
                std::string hash;

                if (!(ls >> ino >> hash)) continue;

                auto pit = ino_to_path.find(ino);
                if (pit == ino_to_path.end()) continue;

                // Look up the inode by path and add the chunk hash.
                auto existing = inode_table.LookupByPath(pit->second);
                if (existing) {
                    FileInode updated = *existing;
                    updated.chunk_hashes.push_back(hash);
                    inode_table.CreateFile(pit->second, updated);
                }
            }
        }

        return true;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// FuseFileSystem Public API
// ─────────────────────────────────────────────────────────────────────────────

FuseFileSystem::FuseFileSystem(FuseConfig config,
                               std::shared_ptr<dht::KademliaDht> dht,
                               std::shared_ptr<cas::CasStore> cas_store)
    : impl_(std::make_unique<Impl>(std::move(config),
                                    std::move(dht),
                                    std::move(cas_store))) {}

FuseFileSystem::~FuseFileSystem() {
    Unmount();
}

bool FuseFileSystem::Mount() {
    if (impl_->mounted.load()) return false;

    // Ensure the mount point directory exists.
    std::filesystem::create_directories(impl_->config.mount_point);

    // Start the chunk fetcher (TCP chunk server + DHT integration).
    if (!impl_->chunk_fetcher->Start()) {
        std::cerr << "[zm-vfs] failed to start chunk fetcher\n";
        return false;
    }

    // Attempt to load existing manifest from DHT.
    if (impl_->LoadManifestFromDht()) {
        std::cout << "[zm-vfs] loaded filesystem manifest from DHT\n";
    }

    impl_->mounted.store(true);

    // In a full implementation, this would launch the FUSE event loop:
    //   impl_->fuse_thread = std::thread([this]() { RunFuseLoop(); });
    // For now, the VFS operates as an in-process filesystem abstraction
    // that higher-level FUSE callbacks can delegate to.

    std::cout << "[zm-vfs] mounted at " << impl_->config.mount_point
              << " (chunk_size=" << impl_->config.chunk_size_bytes
              << ", cache_max=" << impl_->config.max_cache_bytes << ")\n";

    return true;
}

void FuseFileSystem::Unmount() {
    if (!impl_->mounted.exchange(false)) return;

    // Publish final manifest to DHT for persistence.
    impl_->PublishManifestToDht();

    // Stop the chunk fetcher.
    impl_->chunk_fetcher->Stop();

    // Join FUSE thread if running.
    if (impl_->fuse_thread.joinable()) {
        impl_->fuse_thread.join();
    }

    std::cout << "[zm-vfs] unmounted from " << impl_->config.mount_point << "\n";
}

bool FuseFileSystem::IsMounted() const {
    return impl_->mounted.load();
}

std::string FuseFileSystem::PublishFile(const std::filesystem::path& local_src,
                                         std::string_view virtual_dest) {
    if (!std::filesystem::exists(local_src)) {
        std::cerr << "[zm-vfs] publish: source file not found: " << local_src << "\n";
        return {};
    }

    if (!std::filesystem::is_regular_file(local_src)) {
        std::cerr << "[zm-vfs] publish: not a regular file: " << local_src << "\n";
        return {};
    }

    // Chunk the file and store/announce each chunk.
    auto chunk_hashes = impl_->ChunkAndPublish(local_src);
    if (chunk_hashes.empty()) {
        std::cerr << "[zm-vfs] publish: failed to chunk file: " << local_src << "\n";
        return {};
    }

    // Create a virtual inode.
    FileInode inode;
    inode.type = InodeType::RegularFile;
    inode.mode = 0644;
    inode.size_bytes = std::filesystem::file_size(local_src);
    inode.chunk_hashes = std::move(chunk_hashes);
    inode.mtime_sec = NowSec();
    inode.ctime_sec = inode.mtime_sec;
    inode.atime_sec = inode.mtime_sec;

    std::string norm_path = NormalizePath(virtual_dest);
    impl_->inode_table.CreateFile(norm_path, inode);

    impl_->stats.files_published.fetch_add(1);

    // Compute a root hash over all chunk hashes for identification.
    Sha256Engine root_hasher;
    for (const auto& h : inode.chunk_hashes) {
        root_hasher.Update(reinterpret_cast<const uint8_t*>(h.data()), h.size());
    }
    std::string root_hash = root_hasher.Final();

    std::cout << "[zm-vfs] published " << local_src.filename().string()
              << " → " << norm_path
              << " (" << inode.chunk_hashes.size() << " chunks, "
              << inode.size_bytes << " bytes)\n";

    return root_hash;
}

size_t FuseFileSystem::PublishDirectory(const std::filesystem::path& local_dir,
                                         std::string_view virtual_dest) {
    if (!std::filesystem::exists(local_dir) || !std::filesystem::is_directory(local_dir)) {
        std::cerr << "[zm-vfs] publish_dir: not a directory: " << local_dir << "\n";
        return 0;
    }

    std::string base_virt = std::string(virtual_dest);
    size_t published_count = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(local_dir)) {
        if (!entry.is_regular_file()) continue;

        // Compute relative path from local_dir.
        auto rel = std::filesystem::relative(entry.path(), local_dir);
        std::string virt_path = base_virt;
        if (virt_path.back() != '/') virt_path += '/';
        virt_path += rel.string();

        std::string result = PublishFile(entry.path(), virt_path);
        if (!result.empty()) {
            ++published_count;
        }
    }

    std::cout << "[zm-vfs] published directory " << local_dir
              << " → " << virtual_dest
              << " (" << published_count << " files)\n";

    return published_count;
}

std::optional<FileInode> FuseFileSystem::Lookup(std::string_view path) const {
    return impl_->inode_table.LookupByPath(NormalizePath(path));
}

std::vector<DirEntry> FuseFileSystem::ReadDir(std::string_view path) const {
    return impl_->inode_table.ListDirectoryByPath(NormalizePath(path));
}

const VfsStats& FuseFileSystem::GetStats() const {
    return impl_->stats;
}

} // namespace zeromesh::vfs
