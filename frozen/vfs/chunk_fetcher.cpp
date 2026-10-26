#include "vfs/chunk_fetcher.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace zeromesh::vfs {

// ─────────────────────────────────────────────────────────────────────────────
// ChunkCache Implementation
// ─────────────────────────────────────────────────────────────────────────────

ChunkCache::ChunkCache(std::filesystem::path cache_dir, size_t max_bytes)
    : cache_dir_(std::move(cache_dir))
    , max_bytes_(max_bytes) {
    std::filesystem::create_directories(cache_dir_);

    // Rebuild LRU from existing cached files on startup.
    if (std::filesystem::exists(cache_dir_)) {
        // Collect entries sorted by last modified time (oldest first).
        struct CachedFile {
            std::string hash;
            size_t size;
            std::filesystem::file_time_type mtime;
        };
        std::vector<CachedFile> existing;

        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
            if (entry.is_regular_file()) {
                existing.push_back({
                    entry.path().filename().string(),
                    static_cast<size_t>(entry.file_size()),
                    entry.last_write_time()
                });
            }
        }

        // Sort by mtime ascending so the most recent ends up at LRU front.
        std::sort(existing.begin(), existing.end(),
                  [](const CachedFile& a, const CachedFile& b) {
                      return a.mtime < b.mtime;
                  });

        for (const auto& cf : existing) {
            CacheEntry ce{cf.hash, cf.size};
            lru_list_.push_front(ce);
            lru_map_[cf.hash] = lru_list_.begin();
            current_bytes_ += cf.size;
        }
    }
}

std::optional<std::string> ChunkCache::Get(const std::string& chunk_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lru_map_.find(chunk_hash);
    if (it == lru_map_.end()) return std::nullopt;

    // Promote to front of LRU.
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);

    // Read from disk.
    auto path = cache_dir_ / chunk_hash;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;

    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void ChunkCache::Put(const std::string& chunk_hash, const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    // If already present, update and promote.
    auto it = lru_map_.find(chunk_hash);
    if (it != lru_map_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return;
    }

    // Evict until we have space.
    while (current_bytes_ + data.size() > max_bytes_ && !lru_list_.empty()) {
        Evict();
    }

    // Write chunk to disk.
    auto path = cache_dir_ / chunk_hash;
    auto tmp_path = cache_dir_ / (chunk_hash + ".tmp");
    {
        std::ofstream out(tmp_path, std::ios::binary);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::filesystem::rename(tmp_path, path);

    CacheEntry entry{chunk_hash, data.size()};
    lru_list_.push_front(entry);
    lru_map_[chunk_hash] = lru_list_.begin();
    current_bytes_ += data.size();
}

bool ChunkCache::Contains(const std::string& chunk_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lru_map_.contains(chunk_hash);
}

size_t ChunkCache::CurrentBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bytes_;
}

uint64_t ChunkCache::EvictionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return eviction_count_;
}

void ChunkCache::Evict() {
    // Caller must hold mutex_.
    if (lru_list_.empty()) return;

    auto& victim = lru_list_.back();
    auto victim_path = cache_dir_ / victim.hash;
    std::filesystem::remove(victim_path);

    current_bytes_ -= victim.size_bytes;
    lru_map_.erase(victim.hash);
    lru_list_.pop_back();
    ++eviction_count_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Portable SHA-256 for chunk integrity verification
// ─────────────────────────────────────────────────────────────────────────────

namespace {

class Sha256Verifier {
public:
    Sha256Verifier() { Reset(); }

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

/// Compute SHA-256 hex digest for in-memory data.
std::string ComputeSha256(const std::string& data) {
    Sha256Verifier hasher;
    hasher.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return hasher.Final();
}

/// Convert a SHA-256 hex string to a DHT Key (32-byte array).
dht::Key HexToKey(const std::string& hex) {
    dht::Key key{};
    for (size_t i = 0; i < 32 && i * 2 + 1 < hex.size(); ++i) {
        auto hi = hex[i * 2];
        auto lo = hex[i * 2 + 1];
        auto hex_digit = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
            return 0;
        };
        key[i] = static_cast<uint8_t>((hex_digit(hi) << 4) | hex_digit(lo));
    }
    return key;
}

/// Set a socket to non-blocking mode.
bool SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// ChunkFetcher::Impl — PIMPL internals
// ─────────────────────────────────────────────────────────────────────────────

struct ChunkFetcher::Impl {
    ChunkFetcherConfig config;
    std::shared_ptr<dht::KademliaDht> dht;
    std::shared_ptr<cas::CasStore> cas_store;
    std::shared_ptr<ChunkCache> cache;

    std::atomic<bool> running{false};
    std::thread server_thread;

    int server_fd{-1};

    // ── Chunk Serving (TCP server) ──────────────────────────────────────

    /**
     * Minimal TCP chunk server protocol:
     * Client sends: 64-byte hex chunk hash + '\n'
     * Server responds:
     *   On success: "OK <size>\n" followed by <size> raw bytes
     *   On failure: "ERR NOT_FOUND\n"
     */
    void ServerLoop() {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "[zm-vfs] chunk server: socket() failed: "
                      << strerror(errno) << "\n";
            return;
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(config.local_serve_port);

        if (bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[zm-vfs] chunk server: bind() failed on port "
                      << config.local_serve_port << ": " << strerror(errno) << "\n";
            close(server_fd);
            server_fd = -1;
            return;
        }

        if (listen(server_fd, 32) < 0) {
            std::cerr << "[zm-vfs] chunk server: listen() failed: "
                      << strerror(errno) << "\n";
            close(server_fd);
            server_fd = -1;
            return;
        }

        SetNonBlocking(server_fd);

        while (running.load()) {
            // Poll for incoming connections with a 500ms timeout.
            struct pollfd pfd{};
            pfd.fd = server_fd;
            pfd.events = POLLIN;

            int ret = poll(&pfd, 1, 500);
            if (ret <= 0) continue;

            struct sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd,
                                   reinterpret_cast<struct sockaddr*>(&client_addr),
                                   &client_len);
            if (client_fd < 0) continue;

            // Handle the request in-line (single-threaded for simplicity;
            // production would use a thread pool or epoll).
            HandleChunkRequest(client_fd);
            close(client_fd);
        }

        close(server_fd);
        server_fd = -1;
    }

    void HandleChunkRequest(int client_fd) {
        // Read the request: 64-char hex hash + newline.
        char buf[128];
        ssize_t n = 0;
        size_t total = 0;

        // Read with timeout.
        struct pollfd pfd{};
        pfd.fd = client_fd;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, 2000) <= 0) return;

        n = read(client_fd, buf, sizeof(buf) - 1);
        if (n <= 0) return;
        total = static_cast<size_t>(n);
        buf[total] = '\0';

        // Strip trailing newline.
        std::string hash_str(buf);
        while (!hash_str.empty() && (hash_str.back() == '\n' || hash_str.back() == '\r')) {
            hash_str.pop_back();
        }

        if (hash_str.size() != 64) {
            std::string err = "ERR INVALID_HASH\n";
            write(client_fd, err.data(), err.size());
            return;
        }

        // Look up in CAS store first, then chunk cache.
        std::optional<std::string> data = cas_store->GetBlob(hash_str);
        if (!data) {
            data = cache->Get(hash_str);
        }

        if (!data) {
            std::string err = "ERR NOT_FOUND\n";
            write(client_fd, err.data(), err.size());
            return;
        }

        // Send response: "OK <size>\n" + raw bytes.
        std::string header = "OK " + std::to_string(data->size()) + "\n";
        write(client_fd, header.data(), header.size());

        // Send chunk data in segments to handle large chunks.
        const char* ptr = data->data();
        size_t remaining = data->size();
        while (remaining > 0) {
            ssize_t sent = write(client_fd, ptr, remaining);
            if (sent <= 0) break;
            ptr += sent;
            remaining -= static_cast<size_t>(sent);
        }
    }

    // ── Chunk Fetching (TCP client) ─────────────────────────────────────

    /**
     * Fetches a chunk from a specific peer over TCP.
     * Returns the raw chunk bytes on success, or std::nullopt on failure.
     */
    std::optional<std::string> FetchFromPeer(const std::string& ip,
                                              uint16_t port,
                                              const std::string& chunk_hash) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return std::nullopt;

        struct sockaddr_in peer_addr{};
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &peer_addr.sin_addr) <= 0) {
            close(sock);
            return std::nullopt;
        }

        // Set TCP_NODELAY for low-latency request/response.
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        // Non-blocking connect with timeout.
        SetNonBlocking(sock);
        int conn_ret = connect(sock, reinterpret_cast<struct sockaddr*>(&peer_addr),
                               sizeof(peer_addr));
        if (conn_ret < 0 && errno != EINPROGRESS) {
            close(sock);
            return std::nullopt;
        }

        if (conn_ret < 0) {
            // Wait for connection with timeout.
            struct pollfd pfd{};
            pfd.fd = sock;
            pfd.events = POLLOUT;
            int poll_ret = poll(&pfd, 1,
                                static_cast<int>(config.fetch_timeout.count()));
            if (poll_ret <= 0) {
                close(sock);
                return std::nullopt;
            }

            int err = 0;
            socklen_t err_len = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len);
            if (err != 0) {
                close(sock);
                return std::nullopt;
            }
        }

        // Send the hash request: "<64-char hex hash>\n"
        std::string request = chunk_hash + "\n";
        ssize_t sent = send(sock, request.data(), request.size(), 0);
        if (sent <= 0) {
            close(sock);
            return std::nullopt;
        }

        // Read the response header: "OK <size>\n" or "ERR ...\n"
        // Read into buffer until we get a newline.
        std::string resp_header;
        {
            struct pollfd pfd{};
            pfd.fd = sock;
            pfd.events = POLLIN;

            char c;
            while (resp_header.size() < 128) {
                int pr = poll(&pfd, 1, static_cast<int>(config.fetch_timeout.count()));
                if (pr <= 0) {
                    close(sock);
                    return std::nullopt;
                }
                ssize_t r = recv(sock, &c, 1, 0);
                if (r <= 0) {
                    close(sock);
                    return std::nullopt;
                }
                if (c == '\n') break;
                resp_header += c;
            }
        }

        if (resp_header.substr(0, 2) != "OK") {
            close(sock);
            return std::nullopt;
        }

        // Parse size from "OK <size>"
        size_t expected_size = 0;
        try {
            expected_size = std::stoull(resp_header.substr(3));
        } catch (...) {
            close(sock);
            return std::nullopt;
        }

        // Read exactly expected_size bytes of chunk data.
        std::string chunk_data;
        chunk_data.reserve(expected_size);
        size_t received = 0;

        while (received < expected_size) {
            struct pollfd pfd{};
            pfd.fd = sock;
            pfd.events = POLLIN;

            int pr = poll(&pfd, 1, static_cast<int>(config.fetch_timeout.count()));
            if (pr <= 0) break;

            char buf[64 * 1024];
            size_t to_read = std::min(sizeof(buf), expected_size - received);
            ssize_t r = recv(sock, buf, to_read, 0);
            if (r <= 0) break;

            chunk_data.append(buf, static_cast<size_t>(r));
            received += static_cast<size_t>(r);
        }

        close(sock);

        if (received != expected_size) return std::nullopt;

        return chunk_data;
    }

    /**
     * Full chunk fetch pipeline: cache → CAS → DHT providers → TCP download.
     */
    ChunkResult DoFetchChunk(const std::string& chunk_hash) {
        ChunkResult result{};
        auto start_time = std::chrono::steady_clock::now();

        // 1. Check chunk cache (hot path).
        auto cached = cache->Get(chunk_hash);
        if (cached) {
            result.success = true;
            result.data = std::move(*cached);
            result.source_peer_ip = "local-cache";
            result.rtt = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_time);
            return result;
        }

        // 2. Check CAS store (local disk).
        auto cas_blob = cas_store->GetBlob(chunk_hash);
        if (cas_blob) {
            // Cache it for future reads.
            cache->Put(chunk_hash, *cas_blob);

            result.success = true;
            result.data = std::move(*cas_blob);
            result.source_peer_ip = "local-cas";
            result.rtt = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start_time);
            return result;
        }

        // 3. Query DHT for chunk providers.
        dht::Key dht_key = HexToKey(chunk_hash);
        auto providers = dht->GetChunkProviders(dht_key);

        if (providers.empty()) {
            result.success = false;
            return result;
        }

        // 4. Try each provider in order (sorted by RTT).
        std::sort(providers.begin(), providers.end(),
                  [](const dht::DhtNode& a, const dht::DhtNode& b) {
                      return a.rtt_us < b.rtt_us;
                  });

        for (size_t attempt = 0; attempt < config.max_retries; ++attempt) {
            for (const auto& provider : providers) {
                uint16_t target_port = provider.port > 0 ? provider.port : config.remote_chunk_port;
                auto fetched = FetchFromPeer(provider.ip_address,
                                              target_port,
                                              chunk_hash);
                if (!fetched) continue;

                // 5. Verify SHA-256 integrity.
                std::string computed_hash = ComputeSha256(*fetched);
                if (computed_hash != chunk_hash) {
                    std::cerr << "[zm-vfs] chunk integrity mismatch from "
                              << provider.ip_address << " for " << chunk_hash
                              << " (got " << computed_hash << ")\n";
                    continue;
                }

                // 6. Store verified chunk locally.
                cas_store->PutBlob(*fetched);
                cache->Put(chunk_hash, *fetched);

                result.success = true;
                result.data = std::move(*fetched);
                result.source_peer_ip = provider.ip_address;
                result.rtt = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start_time);
                return result;
            }
        }

        result.success = false;
        return result;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// ChunkFetcher Public API
// ─────────────────────────────────────────────────────────────────────────────

ChunkFetcher::ChunkFetcher(ChunkFetcherConfig config,
                           std::shared_ptr<dht::KademliaDht> dht,
                           std::shared_ptr<cas::CasStore> cas_store,
                           std::shared_ptr<ChunkCache> cache)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->dht = std::move(dht);
    impl_->cas_store = std::move(cas_store);
    impl_->cache = std::move(cache);
}

ChunkFetcher::~ChunkFetcher() {
    Stop();
}

bool ChunkFetcher::Start() {
    if (impl_->running.load()) return false;
    impl_->running.store(true);

    // Start the chunk server thread.
    impl_->server_thread = std::thread(&Impl::ServerLoop, impl_.get());
    return true;
}

void ChunkFetcher::Stop() {
    if (!impl_->running.exchange(false)) return;

    // Shutdown the server socket to unblock accept().
    if (impl_->server_fd >= 0) {
        shutdown(impl_->server_fd, SHUT_RDWR);
    }

    if (impl_->server_thread.joinable()) {
        impl_->server_thread.join();
    }
}

ChunkResult ChunkFetcher::FetchChunk(const std::string& chunk_hash) {
    return impl_->DoFetchChunk(chunk_hash);
}

void ChunkFetcher::FetchChunkAsync(const std::string& chunk_hash,
                                    std::function<void(ChunkResult)> callback) {
    // Launch the fetch on a detached thread.
    // In production this would use a bounded thread pool.
    std::thread([impl = impl_.get(), chunk_hash, cb = std::move(callback)]() {
        auto result = impl->DoFetchChunk(chunk_hash);
        cb(std::move(result));
    }).detach();
}

void ChunkFetcher::PrefetchChunks(const std::vector<std::string>& chunk_hashes) {
    for (const auto& hash : chunk_hashes) {
        // Only prefetch if not already cached.
        if (impl_->cache->Contains(hash)) continue;
        if (impl_->cas_store->HasBlob(hash)) continue;

        FetchChunkAsync(hash, [](ChunkResult) {
            // Prefetch is fire-and-forget; result stored in cache.
        });
    }
}

void ChunkFetcher::AnnounceLocalChunk(const std::string& chunk_hash) {
    dht::Key key = HexToKey(chunk_hash);
    impl_->dht->AnnounceChunk(key, impl_->config.local_serve_port);
}

ChunkResult ChunkFetcher::FetchFromPeerDirect(const std::string& ip, uint16_t port, const std::string& chunk_hash) {
    ChunkResult result{};
    auto start_time = std::chrono::steady_clock::now();

    // 1. Check chunk cache
    auto cached = impl_->cache->Get(chunk_hash);
    if (cached) {
        result.success = true;
        result.data = std::move(*cached);
        result.source_peer_ip = "local-cache";
        result.rtt = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }

    // 2. Check CAS store
    auto cas_blob = impl_->cas_store->GetBlob(chunk_hash);
    if (cas_blob) {
        impl_->cache->Put(chunk_hash, *cas_blob);
        result.success = true;
        result.data = std::move(*cas_blob);
        result.source_peer_ip = "local-cas";
        result.rtt = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start_time);
        return result;
    }

    // 3. Directly fetch from given peer
    auto fetched = impl_->FetchFromPeer(ip, port, chunk_hash);
    if (!fetched) {
        result.success = false;
        return result;
    }

    // 4. Verify SHA-256 integrity
    std::string computed = ComputeSha256(*fetched);
    if (computed != chunk_hash) {
        result.success = false;
        return result;
    }

    // 5. Store verified chunk locally
    impl_->cas_store->PutBlob(*fetched);
    impl_->cache->Put(chunk_hash, *fetched);

    result.success = true;
    result.data = std::move(*fetched);
    result.source_peer_ip = ip;
    result.rtt = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start_time);
    return result;
}

} // namespace zeromesh::vfs
