#include "store/stat_cache.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <vector>

namespace zeromesh::store {

namespace {

constexpr int64_t kRacyWindowNs = 1'000'000'000;

int64_t Ns(const struct timespec& ts) {
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

StatCache::StatCache(std::filesystem::path file) : file_(std::move(file)) {
    std::ifstream in(file_);
    std::string line;
    while (std::getline(in, line)) {
        // path \t dev \t ino \t size \t mtime_ns \t ctime_ns \t hashed_at_ns \t hash \t blob_size
        std::vector<std::string> f;
        size_t start = 0;
        for (;;) {
            size_t tab = line.find('\t', start);
            f.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (f.size() != 9 || f[0].empty() || !CasStore::IsValidHash(f[7])) continue;
        try {
            Entry e;
            e.dev = std::stoull(f[1]);
            e.ino = std::stoull(f[2]);
            e.size = std::stoll(f[3]);
            e.mtime_ns = std::stoll(f[4]);
            e.ctime_ns = std::stoll(f[5]);
            e.hashed_at_ns = std::stoll(f[6]);
            e.digest = Digest{f[7], std::stoll(f[8])};
            entries_[f[0]] = std::move(e);
        } catch (const std::exception&) {
            continue; // one bad line costs one rehash
        }
    }
}

bool StatCache::Matches(const Entry& e, const struct stat& st) {
    return e.dev == static_cast<uint64_t>(st.st_dev) && e.ino == static_cast<uint64_t>(st.st_ino) &&
           e.size == static_cast<int64_t>(st.st_size) && e.mtime_ns == Ns(st.st_mtim) && e.ctime_ns == Ns(st.st_ctim) &&
           e.mtime_ns < e.hashed_at_ns - kRacyWindowNs;
}

std::optional<Digest> StatCache::Lookup(const std::string& path, const struct stat& st) {
    std::lock_guard lock(mu_);
    auto it = entries_.find(path);
    if (it == entries_.end() || !Matches(it->second, st)) {
        ++misses_;
        return std::nullopt;
    }
    it->second.used = true;
    ++hits_;
    return it->second.digest;
}

void StatCache::Store(const std::string& path, const struct stat& st, const Digest& digest) {
    if (path.find_first_of("\t\n") != std::string::npos) return;
    Entry e;
    e.dev = static_cast<uint64_t>(st.st_dev);
    e.ino = static_cast<uint64_t>(st.st_ino);
    e.size = static_cast<int64_t>(st.st_size);
    e.mtime_ns = Ns(st.st_mtim);
    e.ctime_ns = Ns(st.st_ctim);
    e.hashed_at_ns = NowNs();
    e.digest = digest;
    e.used = true;
    std::lock_guard lock(mu_);
    entries_[path] = std::move(e);
}

void StatCache::Save() {
    if (file_.empty()) return;
    std::ostringstream out;
    {
        std::lock_guard lock(mu_);
        for (const auto& [path, e] : entries_) {
            if (!e.used) continue;
            out << path << '\t' << e.dev << '\t' << e.ino << '\t' << e.size << '\t' << e.mtime_ns << '\t' << e.ctime_ns
                << '\t' << e.hashed_at_ns << '\t' << e.digest.hash << '\t' << e.digest.size_bytes << '\n';
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    const auto tmp = file_.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << out.str();
        if (!f) return;
    }
    std::filesystem::rename(tmp, file_, ec);
}

size_t StatCache::hits() const {
    std::lock_guard lock(mu_);
    return hits_;
}

size_t StatCache::misses() const {
    std::lock_guard lock(mu_);
    return misses_;
}

} // namespace zeromesh::store
