#include "plugins/env-tree/env_tree.hpp"

#include "plugins/env-tree/chunk_source.hpp"
#include "plugins/env-tree/env_profile.hpp"
#include "plugins/env-tree/mirror_cache.hpp"
#include "plugins/env-tree/mirror_fuse.hpp"
#include "node/env_capture.hpp"
#include "plugins/env-tree/mirror_prefetch.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace zeromesh::tree {

namespace {

namespace fs = std::filesystem;

// Environments fetched whole and kept for later batches. Older unused ones are dropped.
constexpr size_t kKeptTrees = 4;

std::string Param(const plugin::EnvParams& p, const char* key) {
    auto it = p.params.find(key);
    return it == p.params.end() ? std::string() : it->second;
}


// Writes the tree `hash` under `dest`, reading every manifest and blob through `src`.
bool FetchTree(plugin::IBlobSource& src, const std::string& hash, const fs::path& dest) {
    auto data = src.FetchBlob(hash);
    if (!data) return false;
    auto m = store::DirectoryManifest::Deserialize(*data);
    if (!m) return false;
    std::error_code ec;
    fs::create_directories(dest, ec);
    for (const auto& f : m->files) {
        if (!store::MerkleBuilder::IsSafeName(f.name)) return false;
        auto blob = src.FetchBlob(f.digest.hash);
        if (!blob) return false;
        const fs::path p = dest / f.name;
        {
            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            out.write(blob->data(), static_cast<std::streamsize>(blob->size()));
            if (!out) return false;
        }
        fs::permissions(p, f.is_executable ? fs::perms(0755) : fs::perms(0644), ec);
    }
    for (const auto& d : m->directories) {
        if (!store::MerkleBuilder::IsSafeName(d.name) || !FetchTree(src, d.digest.hash, dest / d.name)) return false;
    }
    for (const auto& l : m->symlinks) {
        if (!store::MerkleBuilder::IsSafeName(l.name)) return false;
        fs::create_symlink(l.target, dest / l.name, ec);
        if (ec) return false;
    }
    return true;
}

// One environment in use by one batch.
struct Active {
    std::string root;
    std::shared_ptr<MirrorFuse> fuse; // lazily mounted
    fs::path fetched;                 // or fetched whole
    fs::path profile_dir;
    std::shared_ptr<AccessRecorder> recorder;     // first run of a tree: record what it reads
    std::unique_ptr<BackgroundPrefetch> prefetch; // later runs: prefetch it
};

class TreeEnv final : public plugin::IEnvProvider {
public:
    explicit TreeEnv(bool allow_fuse) : allow_fuse_(allow_fuse) {}
    ~TreeEnv() override {
        std::lock_guard lock(mu_);
        for (auto& [id, a] : active_) {
            a.prefetch.reset();
            if (a.fuse) a.fuse->Unmount();
        }
    }

    std::string_view Name() const override { return "tree"; }

    plugin::Availability Check(const plugin::EnvParams& params) override {
        if (!store::CasStore::IsValidHash(Param(params, plugin::kTreeParam))) {
            return {.ok = false, .reason = "tree environment without a valid tree digest"};
        }
        return {.ok = true, .reason = {}};
    }

    plugin::Result<plugin::SandboxEnv> Prepare(const plugin::BatchContext& ctx) override {
        const std::string root = Param(ctx.env, plugin::kTreeParam);
        if (!store::CasStore::IsValidHash(root)) return {.value = std::nullopt, .error = "no tree digest"};
        if (!ctx.blob_source) return {.value = std::nullopt, .error = "no way to fetch the environment"};
        if (ctx.batch_id.empty()) return {.value = std::nullopt, .error = "no batch id"};

        Active a;
        a.root = root;
        fs::path view; // the tree, as a directory on this node
        if (allow_fuse_ && MirrorFuse::IsFuseAvailable()) {
            const fs::path base = ctx.scratch_dir.empty() ? fs::temp_directory_path() / ("zm-tree-" + root.substr(0, 16))
                                                          : ctx.scratch_dir;
            const fs::path mount_dir = base / "tree";
            std::error_code ec;
            // A batch killed mid-run leaves a dead FUSE mount (stat: ENOTCONN); clear only that.
            struct stat st{};
            if (::stat(mount_dir.c_str(), &st) != 0 && errno == ENOTCONN) MirrorFuse::FusermountUnmountLazy(mount_dir);
            fs::create_directories(mount_dir, ec);
            fs::create_directories(base / "tree_cache", ec);
            auto cache = std::make_shared<MirrorCache>(base / "tree_cache", 512 * 1024 * 1024);
            a.fuse = std::make_shared<MirrorFuse>(mount_dir, std::make_shared<BlobSourceChunkSource>(ctx.blob_source),
                                                  cache);
            std::optional<EnvProfile> profile;
            if (!ctx.profile_dir.empty()) {
                a.profile_dir = ctx.profile_dir;
                ProfileStore profiles(ctx.profile_dir);
                if (auto id = Param(ctx.env, plugin::kProfileParam); !id.empty()) profile = profiles.LoadById(id);
                if (!profile) profile = profiles.LoadForRoot(root);
                if (profile && profile->root_hash != root) profile.reset();
                if (!profile) {
                    a.recorder = std::make_shared<AccessRecorder>();
                    a.fuse->SetRecorder(a.recorder);
                }
            }
            if (!a.fuse->Mount(root)) return {.value = std::nullopt, .error = "cannot mount the environment (FUSE)"};
            if (profile) a.prefetch = std::make_unique<BackgroundPrefetch>(std::move(*profile), a.fuse->Reader());
            view = mount_dir;
        } else {
            auto fetched = FetchWhole(ctx, root);
            if (!fetched) return {.value = std::nullopt, .error = "cannot fetch the environment " + root};
            a.fetched = *fetched;
            view = *fetched;
        }

        plugin::SandboxEnv env;
        std::error_code ec;
        bool has_usr = false;
        for (fs::directory_iterator it(view, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (!node::EnvTreeMounts(name) || !it->is_directory(ec) || it->is_symlink(ec)) continue;
            if (name == "usr") has_usr = true;
            env.binds.push_back({.host = it->path().string(), .sandbox = "/" + name, .read_only = true});
        }
        if (!has_usr) env.binds.insert(env.binds.begin(), {.host = "/usr", .sandbox = "/usr", .read_only = true});
        // bin, lib, ... the tree did not provide as directories follow the host's (usually symlinks into /usr).
        for (const char* p : {"/bin", "/sbin", "/lib", "/lib32", "/lib64"}) {
            if (fs::is_directory(view / (p + 1), ec) && !fs::is_symlink(view / (p + 1), ec)) continue;
            if (fs::is_symlink(p, ec)) {
                const fs::path target = fs::read_symlink(p, ec);
                if (!ec) env.symlinks.push_back({target.string(), p});
            } else if (fs::is_directory(p, ec)) {
                env.binds.push_back({p, p, true});
            }
        }
        // The loader cache and certificates: the environment's own when it carries them.
        for (const char* p : node::kEnvTreeEtcPaths) {
            const fs::path mine = view / (p + 1);
            if (fs::exists(mine, ec))
                env.binds.push_back({mine.string(), p, true});
            else if (fs::exists(p, ec))
                env.binds.push_back({p, p, true});
        }
        env.env.emplace_back("PATH", "/usr/local/bin:/usr/bin:/bin");

        std::lock_guard lock(mu_);
        active_[ctx.batch_id] = std::move(a);
        return {.value = std::move(env), .error = {}};
    }

    void Release(const plugin::BatchContext& ctx) override {
        std::optional<Active> a;
        {
            std::lock_guard lock(mu_);
            auto it = active_.find(ctx.batch_id);
            if (it == active_.end()) return;
            a = std::move(it->second);
            active_.erase(it);
            if (!a->fetched.empty()) {
                --uses_[a->root];
                last_used_[a->root] = std::chrono::steady_clock::now();
            }
        }
        a->prefetch.reset(); // stop and join before unmounting
        if (a->fuse) {
            const MirrorStats st = a->fuse->Stats();
            std::cerr << "tree: batch " << ctx.batch_id << ": " << st.read_ops << " reads, " << st.on_demand_fetches
                      << " fetched on demand, " << st.prefetch_fetches << " prefetched\n";
            a->fuse->Unmount();
            if (a->recorder && !a->profile_dir.empty()) {
                ProfileStore(a->profile_dir).Save(a->recorder->Snapshot(a->root));
            }
        }
    }

private:
    // The tree fetched into ctx.cache_dir/<root> (once; concurrent batches wait for the first).
    std::optional<fs::path> FetchWhole(const plugin::BatchContext& ctx, const std::string& root) {
        if (ctx.cache_dir.empty()) return std::nullopt;
        const fs::path dest = ctx.cache_dir / root;
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return !fetching_.contains(root); });
        ++uses_[root];
        last_used_[root] = std::chrono::steady_clock::now();
        std::error_code ec;
        if (fs::exists(dest, ec)) return dest;
        fetching_.insert({root, true});
        lock.unlock();

        const fs::path tmp = ctx.cache_dir / (root + ".partial");
        fs::remove_all(tmp, ec);
        bool ok = FetchTree(*ctx.blob_source, root, tmp);
        if (ok) {
            fs::rename(tmp, dest, ec);
            ok = !ec;
        }
        if (!ok) fs::remove_all(tmp, ec);

        lock.lock();
        fetching_.erase(root);
        cv_.notify_all();
        if (!ok) {
            --uses_[root];
            return std::nullopt;
        }
        EvictLocked(ctx.cache_dir);
        return dest;
    }

    // Keeps at most kKeptTrees fetched trees, dropping the least recently used idle ones.
    void EvictLocked(const fs::path& cache_dir) {
        std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> idle;
        size_t kept = 0;
        for (const auto& [root, n] : uses_) {
            ++kept;
            if (n <= 0) idle.emplace_back(last_used_[root], root);
        }
        std::sort(idle.begin(), idle.end());
        for (const auto& [t, root] : idle) {
            if (kept <= kKeptTrees) break;
            std::error_code ec;
            fs::remove_all(cache_dir / root, ec);
            uses_.erase(root);
            last_used_.erase(root);
            --kept;
        }
    }

    const bool allow_fuse_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Active> active_;
    std::map<std::string, bool> fetching_;
    std::map<std::string, int> uses_;
    std::map<std::string, std::chrono::steady_clock::time_point> last_used_;
};

} // namespace

std::unique_ptr<plugin::IEnvProvider> MakeTreeEnv(bool allow_fuse) {
    return std::make_unique<TreeEnv>(allow_fuse);
}

} // namespace zeromesh::tree
