#include "plugins/env-mirror/mirror_cache.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace zeromesh::mirror {

MirrorCache::MirrorCache(std::filesystem::path cache_dir, size_t max_bytes)
    : cache_dir_(std::move(cache_dir))
    , max_bytes_(max_bytes) {
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
                existing.push_back({
                    entry.path().filename().string(),
                    static_cast<size_t>(entry.file_size()),
                    entry.last_write_time()
                });
            }
        }

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

    while (current_bytes_ + data.size() > max_bytes_ && !lru_list_.empty()) {
        Evict();
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

void MirrorCache::Evict() {
    if (lru_list_.empty()) return;

    auto& victim = lru_list_.back();
    auto victim_path = cache_dir_ / victim.hash;
    std::error_code ec;
    std::filesystem::remove(victim_path, ec);

    current_bytes_ -= victim.size_bytes;
    lru_map_.erase(victim.hash);
    lru_list_.pop_back();
    ++eviction_count_;
}

} // namespace zeromesh::mirror
