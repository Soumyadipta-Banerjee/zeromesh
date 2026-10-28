#include "exec/input_cache.hpp"

#include <sodium.h>

#include <vector>

namespace zeromesh::exec {

namespace {

namespace fs = std::filesystem;

constexpr const char* kEmptyKey = "empty";

// Makes everything owner-writable so remove_all succeeds whatever modes the tree carries.
void ForceRemove(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) {
            fs::permissions(it->path(), fs::perms::owner_all, fs::perm_options::add, ec);
        }
    }
    fs::remove_all(dir, ec);
}

std::string RandomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    randombytes_buf(raw.data(), raw.size());
    std::string out;
    for (unsigned char c : raw) {
        out.push_back("0123456789abcdef"[c >> 4]);
        out.push_back("0123456789abcdef"[c & 15]);
    }
    return out;
}

} // namespace

InputCache::Lease::~Lease() {
    cache_->Release(key_);
}

InputCache::InputCache(fs::path dir, std::shared_ptr<store::MerkleBuilder> merkle)
    : dir_(std::move(dir)), merkle_(std::move(merkle)) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium init failed");
    ForceRemove(dir_);
    fs::create_directories(dir_);
}

std::shared_ptr<InputCache::Lease> InputCache::Acquire(const store::Digest& root) {
    const std::string key = root.hash.empty() ? kEmptyKey : root.hash;
    const fs::path path = dir_ / key;
    {
        std::unique_lock lock(mu_);
        Entry& e = entries_[key]; // map nodes are stable, and an entry with leases is never erased
        ++e.leases;
        e.last_used = std::chrono::steady_clock::now();
        cv_.wait(lock, [&] { return e.ready || !e.materializing; });
        if (e.ready) return std::make_shared<Lease>(this, key, path);
        e.materializing = true; // ours to build; a failed earlier attempt is simply retried
    }

    const fs::path tmp = dir_ / (key + ".tmp-" + RandomHex(4));
    bool ok = true;
    if (root.hash.empty()) {
        std::error_code ec;
        ok = fs::create_directories(tmp, ec);
    } else {
        try {
            ok = merkle_->MaterializeTree(root, tmp, store::Materialize::Link);
        } catch (const std::exception&) {
            ok = false;
        }
    }
    if (ok) {
        std::error_code ec;
        fs::rename(tmp, path, ec);
        ok = !ec;
    }
    if (!ok) ForceRemove(tmp);

    std::lock_guard lock(mu_);
    Entry& e = entries_[key];
    e.materializing = false;
    e.ready = ok;
    cv_.notify_all();
    if (!ok) {
        --e.leases;
        return nullptr;
    }
    return std::make_shared<Lease>(this, key, path);
}

void InputCache::Release(const std::string& key) {
    std::lock_guard lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return;
    --it->second.leases;
    it->second.last_used = std::chrono::steady_clock::now();
    cv_.notify_all();
}

std::shared_ptr<InputCache::Lease> InputCache::AcquireCached(const store::Digest& root) {
    const std::string key = root.hash.empty() ? kEmptyKey : root.hash;
    std::lock_guard lock(mu_);
    auto it = entries_.find(key);
    if (it == entries_.end() || !it->second.ready) return nullptr;
    ++it->second.leases;
    it->second.last_used = std::chrono::steady_clock::now();
    return std::make_shared<Lease>(this, key, dir_ / key);
}

void InputCache::Prune(std::chrono::seconds idle) {
    std::vector<fs::path> doomed;
    {
        std::lock_guard lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            const Entry& e = it->second;
            if (e.leases <= 0 && !e.materializing && (!e.ready || now - e.last_used >= idle)) {
                if (e.ready) {
                    // Detach the old path while locked. A new acquisition can then materialize
                    // the same root while slow removal runs beside it, without losing its tree.
                    const auto removed = dir_ / (it->first + ".prune-" + RandomHex(8));
                    std::error_code ec;
                    fs::rename(dir_ / it->first, removed, ec);
                    if (ec) {
                        ++it;
                        continue;
                    }
                    doomed.push_back(removed);
                }
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& p : doomed) ForceRemove(p);
}

size_t InputCache::Entries() const {
    std::lock_guard lock(mu_);
    size_t n = 0;
    for (const auto& [k, e] : entries_) n += e.ready ? 1 : 0;
    return n;
}

} // namespace zeromesh::exec
