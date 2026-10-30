#include "plugins/env-mirror/mirror_cache.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

namespace zeromesh::mirror {

MirrorCache::MirrorCache(std::filesystem::path cache_dir, size_t max_bytes)
    : cache_dir_(std::move(cache_dir)), max_bytes_(max_bytes) {
    std::filesystem::create_directories(cache_dir_);

    // Rebuild LRU from existing cached files on startup.
    if (std::filesystem::exists(cache_dir_)) {
        struct CachedFile {
            std::string hash;
            size_t size;
            std::filesystem::file_time_type mtime;
        };
        std::vector<CachedFile> existing;

        for (const auto& entry : std::filesystem::directory_iterator(cache_dir_)) {
            if (entry.is_regular_file()) {
                existing.push_back({entry.path().filename().string(), static_cast<size_t>(entry.file_size()),
                                    entry.last_write_time()});
            }
        }

        std::sort(existing.begin(), existing.end(),
                  [](const CachedFile& a, const CachedFile& b) { return a.mtime < b.mtime; });

        for (const auto& cf : existing) {
            CacheEntry ce{cf.hash, cf.size};
            lru_list_.push_front(ce);
            lru_map_[cf.hash] = lru_list_.begin();
            current_bytes_ += cf.size;
        }
    }
}

std::optional<std::string> MirrorCache::Get(const std::string& chunk_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lru_map_.find(chunk_hash);
    if (it == lru_map_.end()) return std::nullopt;

    // Promote to front of LRU.
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);

    auto path = cache_dir_ / chunk_hash;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;

    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void MirrorCache::Put(const std::string& chunk_hash, const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = lru_map_.find(chunk_hash);
    if (it != lru_map_.end()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return;
    }

    // Pinned chunks are never dropped; if nothing else is evictable the cache overshoots its budget.
    while (current_bytes_ + data.size() > max_bytes_ && Evict()) {
    }

    auto path = cache_dir_ / chunk_hash;
    auto tmp_path = cache_dir_ / (chunk_hash + ".tmp");
    {
        std::ofstream out(tmp_path, std::ios::binary);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) return;

    CacheEntry entry{chunk_hash, data.size()};
    lru_list_.push_front(entry);
    lru_map_[chunk_hash] = lru_list_.begin();
    current_bytes_ += data.size();
}

bool MirrorCache::Contains(const std::string& chunk_hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lru_map_.contains(chunk_hash);
}

size_t MirrorCache::CurrentBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_bytes_;
}

uint64_t MirrorCache::EvictionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return eviction_count_;
}

std::optional<size_t> MirrorCache::ReadRange(const std::string& chunk_hash, off_t offset, char* buf, size_t size) {
    if (size == 0) return size_t{0};
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lru_map_.find(chunk_hash);
    if (it == lru_map_.end()) return std::nullopt;
    lru_list_.splice(lru_list_.begin(), lru_list_, it->second);

    auto path = cache_dir_ / chunk_hash;
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    in.seekg(offset, std::ios::beg);
    if (!in) return std::nullopt;
    in.read(buf, static_cast<std::streamsize>(size));
    return static_cast<size_t>(in.gcount());
}

void MirrorCache::Pin(const std::string& chunk_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++pins_[chunk_hash];
}

void MirrorCache::Unpin(const std::string& chunk_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pins_.find(chunk_hash);
    if (it == pins_.end()) return;
    if (--it->second == 0) pins_.erase(it);
}

void MirrorCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cache_dir_, ec)) {
        if (entry.is_regular_file()) std::filesystem::remove(entry.path(), ec);
    }
    lru_list_.clear();
    lru_map_.clear();
    current_bytes_ = 0;
}

bool MirrorCache::Evict() {
    auto victim = std::find_if(lru_list_.rbegin(), lru_list_.rend(),
                               [this](const CacheEntry& e) { return !pins_.contains(e.hash); });
    if (victim == lru_list_.rend()) return false;

    std::error_code ec;
    std::filesystem::remove(cache_dir_ / victim->hash, ec);

    current_bytes_ -= victim->size_bytes;
    lru_map_.erase(victim->hash);
    lru_list_.erase(std::next(victim).base());
    ++eviction_count_;
    return true;
}

} // namespace zeromesh::mirror
