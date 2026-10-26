#include "store/store.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <algorithm>
#include <vector>

namespace zeromesh::store {

namespace {

// Clean portable SHA-256 engine for CAS content addressing
class Sha256Engine {
public:
    Sha256Engine() { Reset(); }

    void Reset() {
        state_[0] = 0x6a09e667;
        state_[1] = 0xbb67ae85;
        state_[2] = 0x3c6ef372;
        state_[3] = 0xa54ff53a;
        state_[4] = 0x510e527f;
        state_[5] = 0x9b05688c;
        state_[6] = 0x1f83d9ab;
        state_[7] = 0x5be0cd19;
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
    static inline uint32_t RoTr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

    void Transform(const uint8_t* block) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        uint32_t W[64];
        for (int t = 0; t < 16; ++t) {
            W[t] = (static_cast<uint32_t>(block[t * 4]) << 24) |
                   (static_cast<uint32_t>(block[t * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[t * 4 + 2]) << 8) |
                   (static_cast<uint32_t>(block[t * 4 + 3]));
        }
        for (int t = 16; t < 64; ++t) {
            uint32_t s0 = RoTr(W[t - 15], 7) ^ RoTr(W[t - 15], 18) ^ (W[t - 15] >> 3);
            uint32_t s1 = RoTr(W[t - 2], 17) ^ RoTr(W[t - 2], 19) ^ (W[t - 2] >> 10);
            W[t] = W[t - 16] + s0 + W[t - 7] + s1;
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

std::chrono::steady_clock::time_point SteadyFromFileTime(std::filesystem::file_time_type ft) {
    const auto sctp = std::chrono::time_point_cast<std::chrono::steady_clock::duration>(
        ft - std::filesystem::file_time_type::clock::now() + std::chrono::steady_clock::now());
    return sctp;
}

} // namespace

CasStore::CasStore(std::filesystem::path base_path, CasLimits limits)
    : limits_(limits) {
    if (base_path.empty()) {
        const char* home = std::getenv("HOME");
        if (home) {
            base_dir_ = std::filesystem::path(home) / ".zeromesh" / "cas";
        } else {
            base_dir_ = std::filesystem::temp_directory_path() / "zeromesh" / "cas";
        }
    } else {
        base_dir_ = std::move(base_path);
    }

    blobs_dir_ = base_dir_ / "blobs";
    ac_dir_ = base_dir_ / "ac";

    std::filesystem::create_directories(blobs_dir_);
    std::filesystem::create_directories(ac_dir_);
    ScanBlobsOnDisk();
}

void CasStore::SetMaxBlobBytes(uint64_t bytes) {
    std::lock_guard lock(store_mutex_);
    limits_.max_blob_bytes = bytes;
}

uint64_t CasStore::MaxBlobBytes() const {
    std::lock_guard lock(store_mutex_);
    return limits_.max_blob_bytes;
}

uint64_t CasStore::BlobBytes() const {
    std::lock_guard lock(store_mutex_);
    return blob_bytes_;
}

uint64_t CasStore::ReservedIncomingBytes() const {
    std::lock_guard lock(store_mutex_);
    return reserved_incoming_;
}

void CasStore::ScanBlobsOnDisk() {
    std::lock_guard lock(store_mutex_);
    blobs_.clear();
    lru_order_.clear();
    lru_iters_.clear();
    blob_bytes_ = 0;

    std::error_code ec;
    if (!std::filesystem::exists(blobs_dir_, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(blobs_dir_, ec)) {
        if (ec || !entry.is_regular_file()) continue;
        const std::string hash = entry.path().filename().string();
        if (!IsValidHash(hash)) continue;
        std::error_code size_ec;
        const auto sz = static_cast<uint64_t>(entry.file_size(size_ec));
        if (size_ec) continue;
        const auto when = SteadyFromFileTime(entry.last_write_time(ec));
        RegisterBlobLocked(hash, sz, when);
    }
}

void CasStore::RegisterBlobLocked(const std::string& hash, uint64_t size,
                                  std::chrono::steady_clock::time_point when) const {
    if (blobs_.contains(hash)) return;
    blobs_[hash] = BlobMeta{size, when};
    lru_order_.push_front(hash);
    lru_iters_[hash] = lru_order_.begin();
    blob_bytes_ += size;
}

void CasStore::PromoteLruLocked(const std::string& hash) const {
    auto it = lru_iters_.find(hash);
    if (it == lru_iters_.end()) return;
    lru_order_.erase(it->second);
    lru_order_.push_front(hash);
    lru_iters_[hash] = lru_order_.begin();
    blobs_[hash].last_used = std::chrono::steady_clock::now();
}

void CasStore::TouchBlobLocked(const std::string& hash) const {
    if (!blobs_.contains(hash)) return;
    PromoteLruLocked(hash);
    TouchBlobOnDisk(blobs_dir_ / hash);
}

void CasStore::TouchBlobOnDisk(const std::filesystem::path& path) const {
    timeval tv[2];
    if (::gettimeofday(&tv[0], nullptr) != 0) return;
    tv[1] = tv[0];
    ::utimes(path.c_str(), tv);
}

void CasStore::DropFromMemCacheLocked(const std::string& hash) const {
    std::lock_guard lock(mem_cache_mutex_);
    auto it = mem_cache_.find(hash);
    if (it == mem_cache_.end()) return;
    cur_mem_bytes_ -= it->second.size();
    mem_cache_.erase(it);
}

void CasStore::EvictBlobMetadataLocked(const std::string& hash) const {
    auto it = blobs_.find(hash);
    if (it == blobs_.end()) return;
    blob_bytes_ -= it->second.size;
    blobs_.erase(it);
    if (auto lit = lru_iters_.find(hash); lit != lru_iters_.end()) {
        lru_order_.erase(lit->second);
        lru_iters_.erase(lit);
    }
}

bool CasStore::DeleteBlobLocked(const std::string& hash) {
    auto it = blobs_.find(hash);
    if (it == blobs_.end()) return false;

    std::error_code ec;
    std::filesystem::remove(blobs_dir_ / hash, ec);
    blob_bytes_ -= it->second.size;
    blobs_.erase(it);
    if (auto lit = lru_iters_.find(hash); lit != lru_iters_.end()) {
        lru_order_.erase(lit->second);
        lru_iters_.erase(lit);
    }
    DropFromMemCacheLocked(hash);
    return true;
}

CasStore::GcStats CasStore::CollectGarbage(uint64_t target_bytes,
                                           const std::unordered_set<std::string>& protect_hashes) {
    GcStats stats;
    std::lock_guard lock(store_mutex_);
    if (blob_bytes_ <= target_bytes) return stats;

    std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> victims;
    victims.reserve(blobs_.size());
    for (const auto& [hash, meta] : blobs_) {
        if (protect_hashes.contains(hash)) continue;
        victims.push_back({meta.last_used, hash});
    }
    std::sort(victims.begin(), victims.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& [_, hash] : victims) {
        if (blob_bytes_ <= target_bytes) break;
        auto it = blobs_.find(hash);
        if (it == blobs_.end()) continue;
        const uint64_t freed = it->second.size;
        if (!DeleteBlobLocked(hash)) continue;
        stats.bytes_freed += freed;
        ++stats.blobs_removed;
    }
    return stats;
}

bool CasStore::CanAdmitIncoming(uint64_t bytes) const {
    std::lock_guard lock(store_mutex_);
    return blob_bytes_ + reserved_incoming_ + bytes <= limits_.max_blob_bytes;
}

bool CasStore::ReserveIncoming(uint64_t bytes) {
    std::lock_guard lock(store_mutex_);
    if (blob_bytes_ + reserved_incoming_ + bytes > limits_.max_blob_bytes) return false;
    reserved_incoming_ += bytes;
    return true;
}

void CasStore::ReleaseIncoming(uint64_t bytes) {
    std::lock_guard lock(store_mutex_);
    reserved_incoming_ = (bytes >= reserved_incoming_) ? 0 : reserved_incoming_ - bytes;
}

void CasStore::NoteBlobCommitted(const std::string& hash, uint64_t size_bytes) {
    if (!IsValidHash(hash)) return;
    std::lock_guard lock(store_mutex_);
    if (blobs_.contains(hash)) {
        PromoteLruLocked(hash);
        return;
    }
    RegisterBlobLocked(hash, size_bytes, std::chrono::steady_clock::now());
    TouchBlobOnDisk(blobs_dir_ / hash);
}

bool CasStore::AdmitPeerIngest(const std::string& peer_id, uint64_t bytes) {
    std::lock_guard lock(store_mutex_);
    if (blob_bytes_ + reserved_incoming_ + bytes > limits_.max_blob_bytes) return false;
    if (peer_id.empty()) return true;

    const auto now = std::chrono::steady_clock::now();
    auto& window = peer_ingest_[peer_id];
    while (!window.events.empty() && now - window.events.front().first > limits_.peer_ingest_window) {
        window.bytes -= window.events.front().second;
        window.events.pop_front();
    }
    return window.bytes + bytes <= limits_.peer_ingest_window_bytes;
}

void CasStore::RecordPeerIngest(const std::string& peer_id, uint64_t bytes) {
    if (peer_id.empty() || bytes == 0) return;
    std::lock_guard lock(store_mutex_);
    const auto now = std::chrono::steady_clock::now();
    auto& window = peer_ingest_[peer_id];
    while (!window.events.empty() && now - window.events.front().first > limits_.peer_ingest_window) {
        window.bytes -= window.events.front().second;
        window.events.pop_front();
    }
    window.bytes += bytes;
    window.events.push_back({now, bytes});
}

bool CasStore::IsValidHash(std::string_view hash) {
    if (hash.size() != 64) return false;
    for (char c : hash) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

Digest CasStore::ComputeDigest(std::string_view data) {
    Sha256Engine ctx;
    ctx.Update(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    return Digest{ctx.Final(), static_cast<int64_t>(data.size())};
}

Digest CasStore::ComputeFileDigest(const std::filesystem::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return Digest{"", 0};
    }

    Sha256Engine ctx;
    std::array<char, 64 * 1024> buf{};
    int64_t total_size = 0;

    while (file.read(buf.data(), buf.size()) || file.gcount() > 0) {
        auto bytes_read = file.gcount();
        total_size += bytes_read;
        ctx.Update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(bytes_read));
    }

    return Digest{ctx.Final(), total_size};
}

void RemovePath(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::filesystem::path MakeUniqueTempIn(const std::filesystem::path& dir, const char* prefix) {
    std::string pattern = (dir / (std::string(prefix) + "XXXXXX")).string();
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "mkstemp " + pattern);
    }
    ::close(fd);
    return std::filesystem::path(buf.data());
}

void FsyncPath(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        throw std::system_error(errno, std::generic_category(), "open for fsync " + path.string());
    }
    if (::fsync(fd) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::system_error(err, std::generic_category(), "fsync " + path.string());
    }
    ::close(fd);
}

void CommitTempFile(const std::filesystem::path& tmp, const std::filesystem::path& target) {
    FsyncPath(tmp);
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (!ec) return;
    if (std::filesystem::exists(target)) {
        RemovePath(tmp);
        return;
    }
    RemovePath(tmp);
    throw std::system_error(ec.value(), std::generic_category(), "rename " + tmp.string() + " -> " + target.string());
}

Digest CasStore::PutBlob(std::string_view data) {
    Digest digest = ComputeDigest(data);

    {
        std::lock_guard lock(mem_cache_mutex_);
        if (data.size() < 1024 * 1024 && cur_mem_bytes_ + data.size() <= max_mem_bytes_) {
            mem_cache_[digest.hash] = std::string(data);
            cur_mem_bytes_ += data.size();
        }
    }

    auto target = blobs_dir_ / digest.hash;
    if (std::filesystem::exists(target)) {
        NoteBlobCommitted(digest.hash, static_cast<uint64_t>(data.size()));
        return digest;
    }

    {
        std::lock_guard lock(store_mutex_);
        if (blob_bytes_ + reserved_incoming_ + data.size() > limits_.max_blob_bytes) {
            throw std::runtime_error("CAS disk quota exceeded");
        }
    }

    auto tmp_target = MakeUniqueTempIn(blobs_dir_, "upload-");
    {
        std::ofstream out(tmp_target, std::ios::binary);
        if (!out.is_open()) {
            RemovePath(tmp_target);
            throw std::runtime_error("cannot write CAS blob temp " + tmp_target.string());
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out.good()) {
            RemovePath(tmp_target);
            throw std::runtime_error("short write to CAS blob temp " + tmp_target.string());
        }
        out.close();
        if (!out.good()) {
            RemovePath(tmp_target);
            throw std::runtime_error("cannot close CAS blob temp " + tmp_target.string());
        }
    }
    try {
        CommitTempFile(tmp_target, target);
    } catch (...) {
        RemovePath(tmp_target);
        throw;
    }
    NoteBlobCommitted(digest.hash, static_cast<uint64_t>(data.size()));
    return digest;
}

Digest CasStore::PutBlobFile(const std::filesystem::path& src_file) {
    std::ifstream in(src_file, std::ios::binary);
    if (!in.is_open()) return Digest{"", 0};

    auto tmp_target = MakeUniqueTempIn(blobs_dir_, "upload-");
    std::ofstream out(tmp_target, std::ios::binary);
    if (!out.is_open()) {
        RemovePath(tmp_target);
        throw std::runtime_error("cannot write CAS blob temp " + tmp_target.string());
    }

    Sha256Engine ctx;
    std::array<char, 64 * 1024> buf{};
    int64_t total_size = 0;
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0) {
        const auto n = in.gcount();
        total_size += n;
        ctx.Update(reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(n));
        out.write(buf.data(), n);
        if (!out.good()) {
            RemovePath(tmp_target);
            throw std::runtime_error("short write to CAS blob temp " + tmp_target.string());
        }
    }
    if (!in.eof() && in.fail()) {
        RemovePath(tmp_target);
        throw std::runtime_error("cannot read " + src_file.string());
    }
    out.close();
    if (!out.good()) {
        RemovePath(tmp_target);
        throw std::runtime_error("cannot close CAS blob temp " + tmp_target.string());
    }

    Digest digest{ctx.Final(), total_size};
    auto target = blobs_dir_ / digest.hash;
    if (std::filesystem::exists(target)) {
        RemovePath(tmp_target);
        NoteBlobCommitted(digest.hash, static_cast<uint64_t>(total_size));
        return digest;
    }

    {
        std::lock_guard lock(store_mutex_);
        if (blob_bytes_ + reserved_incoming_ + static_cast<uint64_t>(total_size) > limits_.max_blob_bytes) {
            RemovePath(tmp_target);
            throw std::runtime_error("CAS disk quota exceeded");
        }
    }

    try {
        CommitTempFile(tmp_target, target);
    } catch (...) {
        RemovePath(tmp_target);
        throw;
    }
    NoteBlobCommitted(digest.hash, static_cast<uint64_t>(total_size));
    return digest;
}

std::optional<std::string> CasStore::GetBlob(const std::string& hash) const {
    if (!IsValidHash(hash)) return std::nullopt;
    {
        std::lock_guard lock(mem_cache_mutex_);
        auto it = mem_cache_.find(hash);
        if (it != mem_cache_.end()) {
            return it->second;
        }
    }

    auto target = blobs_dir_ / hash;
    if (!std::filesystem::exists(target)) {
        return std::nullopt;
    }

    {
        std::lock_guard lock(store_mutex_);
        if (!blobs_.contains(hash)) {
            std::error_code ec;
            const auto sz = static_cast<uint64_t>(std::filesystem::file_size(target, ec));
            if (!ec) {
                RegisterBlobLocked(hash, sz, std::chrono::steady_clock::now());
            }
        }
        TouchBlobLocked(hash);
    }

    std::ifstream in(target, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool CasStore::HasBlob(const std::string& hash) const {
    if (!IsValidHash(hash)) return false;
    auto target = blobs_dir_ / hash;
    if (!std::filesystem::exists(target)) {
        DropFromMemCacheLocked(hash);
        std::lock_guard lock(store_mutex_);
        EvictBlobMetadataLocked(hash);
        return false;
    }
    {
        std::lock_guard lock(store_mutex_);
        if (!blobs_.contains(hash)) {
            std::error_code ec;
            const auto sz = static_cast<uint64_t>(std::filesystem::file_size(target, ec));
            if (!ec) {
                RegisterBlobLocked(hash, sz, std::chrono::steady_clock::now());
            }
        }
        TouchBlobLocked(hash);
    }
    return true;
}

std::filesystem::path CasStore::GetBlobPath(const std::string& hash) const {
    if (!IsValidHash(hash)) return {};
    return blobs_dir_ / hash;
}

bool CasStore::PutActionResult(const std::string& action_hash, std::string_view serialized_result) {
    if (!IsValidHash(action_hash)) return false;
    auto target = ac_dir_ / action_hash;
    if (std::filesystem::exists(target)) return true;
    std::filesystem::path tmp;
    try {
        tmp = MakeUniqueTempIn(ac_dir_, "ac-");
        std::ofstream out(tmp, std::ios::binary);
        if (!out.is_open()) {
            RemovePath(tmp);
            return false;
        }
        out.write(serialized_result.data(), static_cast<std::streamsize>(serialized_result.size()));
        if (!out.good()) {
            RemovePath(tmp);
            return false;
        }
        out.close();
        if (!out.good()) {
            RemovePath(tmp);
            return false;
        }
        CommitTempFile(tmp, target);
        return true;
    } catch (...) {
        if (!tmp.empty()) RemovePath(tmp);
        return false;
    }
}

std::optional<std::string> CasStore::GetActionResult(const std::string& action_hash) const {
    if (!IsValidHash(action_hash)) return std::nullopt;
    auto target = ac_dir_ / action_hash;
    if (!std::filesystem::exists(target)) {
        return std::nullopt;
    }

    std::ifstream in(target, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool CasStore::RemoveActionResult(const std::string& action_hash) {
    if (!IsValidHash(action_hash)) return false;
    std::error_code ec;
    return std::filesystem::remove(ac_dir_ / action_hash, ec);
}

} // namespace zeromesh::store
