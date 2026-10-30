#include "plugins/env-mirror/env_mirror.hpp"

#include "plugins/env-mirror/env_profile.hpp"
#include "plugins/env-mirror/mirror_prefetch.hpp"
#include "store/store.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace zeromesh::mirror {

namespace {

namespace fs = std::filesystem;

bool SafeTaskIdForPath(const std::string& task_id) {
    return !task_id.empty();
}

fs::path MirrorScratchBase(const plugin::TaskContext& ctx) {
    if (!ctx.scratch_dir.empty()) return ctx.scratch_dir;
    return fs::temp_directory_path() / ("zm-mirror-" + store::CasStore::ComputeDigest(ctx.task_id).hash);
}

std::string RootDigestFromParams(const std::map<std::string, std::string>& params) {
    auto it = params.find("root_digest");
    if (it != params.end() && !it->second.empty()) return it->second;
    auto it2 = params.find("manifest_digest");
    if (it2 != params.end() && !it2->second.empty()) return it2->second;
    return {};
}

std::string RootDigest(const plugin::TaskContext& ctx) {
    return RootDigestFromParams(ctx.env.params);
}

// One mounted environment: the mount plus what the access profile needs while it lives.
struct ActiveMount {
    std::shared_ptr<MirrorFuse> fuse;
    std::string root_digest;
    fs::path profile_dir;
    std::shared_ptr<AccessRecorder> recorder;    // set on the first run of an environment (no profile yet)
    std::unique_ptr<BackgroundPrefetch> prefetch; // set when a profile exists
};

class MirrorEnv final : public plugin::IEnvProvider {
public:
    ~MirrorEnv() override {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [task_id, mount] : active_mounts_) {
            mount.prefetch.reset(); // join before the mount goes away
            if (mount.fuse) mount.fuse->Unmount();
        }
        active_mounts_.clear();
    }

    std::string_view Name() const override { return "mirror"; }

    plugin::Availability Check(const plugin::EnvParams& params) override {
        if (!MirrorFuse::IsFuseAvailable()) {
            return {.ok = false, .reason = "FUSE is unavailable: /dev/fuse not accessible"};
        }

        if (params.argv.empty() || params.argv[0].empty()) {
            return {.ok = false, .reason = "empty argv"};
        }

        auto it = params.params.find("requester_unreachable");
        if (it != params.params.end() && it->second == "true") {
            return {.ok = false, .reason = "requester is not reachable"};
        }

        if (RootDigestFromParams(params.params).empty()) {
            return {.ok = false, .reason = "mirror env has no root_digest (build a manifest on the requester)"};
        }

        return {.ok = true, .reason = {}};
    }

    plugin::Result<plugin::SandboxEnv> Prepare(const plugin::TaskContext& ctx) override {
        if (!MirrorFuse::IsFuseAvailable()) {
            return {.value = std::nullopt, .error = "FUSE is not available on this donor"};
        }
        if (!SafeTaskIdForPath(ctx.task_id)) {
            return {.value = std::nullopt, .error = "invalid task_id for mirror scratch path"};
        }

        const std::string root_digest = RootDigest(ctx);
        if (root_digest.empty()) {
            return {.value = std::nullopt, .error = "mirror env has no root_digest"};
        }
        if (!ctx.blob_source) {
            return {.value = std::nullopt, .error = "mirror env has no blob source to fetch files from the requester"};
        }

        fs::path base_scratch = MirrorScratchBase(ctx);
        fs::path mount_dir = base_scratch / "mirror";
        fs::path cache_dir = base_scratch / "mirror_cache";
        std::error_code ec;
        fs::create_directories(mount_dir, ec);
        fs::create_directories(cache_dir, ec);

        auto chunk_source = std::make_shared<BlobSourceChunkSource>(ctx.blob_source);
        auto cache = std::make_shared<MirrorCache>(cache_dir, 512 * 1024 * 1024);
        auto fuse_inst = std::make_shared<MirrorFuse>(mount_dir, chunk_source, cache);

        // First run of this environment: record what the task reads. Later runs: prefetch it.
        ActiveMount mount;
        mount.fuse = fuse_inst;
        mount.root_digest = root_digest;
        mount.profile_dir = ctx.profile_dir;
        std::optional<EnvProfile> profile;
        if (!ctx.profile_dir.empty()) {
            ProfileStore profiles(ctx.profile_dir);
            if (auto it = ctx.env.params.find("profile_id"); it != ctx.env.params.end() && !it->second.empty()) {
                profile = profiles.LoadById(it->second);
            }
            if (!profile) profile = profiles.LoadForRoot(root_digest);
            if (profile && profile->root_hash != root_digest) profile.reset(); // a profile of another environment
            if (!profile) {
                mount.recorder = std::make_shared<AccessRecorder>();
                fuse_inst->SetRecorder(mount.recorder);
            }
        }

        if (!fuse_inst->Mount(root_digest)) {
            return {.value = std::nullopt, .error = "failed to mount FUSE mirror filesystem"};
        }
        if (profile) mount.prefetch = std::make_unique<BackgroundPrefetch>(std::move(*profile), fuse_inst->Reader());

        {
            std::lock_guard<std::mutex> lock(mu_);
            active_mounts_[ctx.task_id] = std::move(mount);
        }

        plugin::SandboxEnv env;
        env.binds.push_back({
            .host = mount_dir.string(),
            .sandbox = "/mirror",
            .read_only = true,
        });

        if (fs::exists(mount_dir / "usr", ec)) {
            env.binds.push_back({
                .host = (mount_dir / "usr").string(),
                .sandbox = "/usr",
                .read_only = true,
            });
        } else {
            env.binds.push_back({
                .host = "/usr",
                .sandbox = "/usr",
                .read_only = true,
            });
        }

        if (fs::exists(mount_dir / "opt", ec)) {
            env.binds.push_back({
                .host = (mount_dir / "opt").string(),
                .sandbox = "/opt",
                .read_only = true,
            });
        }

        for (const char* p : {"/bin", "/sbin", "/lib", "/lib32", "/lib64"}) {
            if (fs::is_symlink(p, ec)) {
                fs::path target = fs::read_symlink(p, ec);
                if (!ec) env.symlinks.push_back({target.string(), p});
            } else if (fs::is_directory(p, ec)) {
                env.binds.push_back({p, p, true});
            }
        }
        for (const char* p : {"/etc/alternatives", "/etc/ld.so.cache", "/etc/ssl"}) {
            if (fs::exists(p, ec)) env.binds.push_back({p, p, true});
        }
        env.env.emplace_back("PATH", "/usr/local/bin:/usr/bin:/bin");

        return {.value = std::move(env), .error = {}};
    }

    void Release(const plugin::TaskContext& ctx) override {
        std::optional<ActiveMount> mount;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = active_mounts_.find(ctx.task_id);
            if (it != active_mounts_.end()) {
                mount = std::move(it->second);
                active_mounts_.erase(it);
            }
        }

        if (mount) {
            mount->prefetch.reset(); // stop and join before unmounting
            if (mount->fuse) {
                const MirrorStats st = mount->fuse->Stats();
                std::cerr << "mirror: task " << ctx.task_id << ": " << st.read_ops << " reads, " << st.on_demand_fetches
                          << " on-demand fetches, " << st.cache_hits << " cache hits, " << st.prefetch_fetches
                          << " prefetched (" << st.prefetch_bytes << " bytes)\n";
                mount->fuse->Unmount();
            }
            if (mount->recorder && !mount->profile_dir.empty()) {
                ProfileStore(mount->profile_dir).Save(mount->recorder->Snapshot(mount->root_digest));
            }
        }

        if (!SafeTaskIdForPath(ctx.task_id)) return;

        fs::path base_scratch = MirrorScratchBase(ctx);
        fs::path mount_dir = base_scratch / "mirror";
        if (MirrorFuse::IsPathMounted(mount_dir)) {
            MirrorFuse::FusermountUnmountLazy(mount_dir);
        }
        std::error_code ec;
        fs::remove_all(base_scratch, ec);
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, ActiveMount> active_mounts_;
};

} // namespace

std::unique_ptr<plugin::IEnvProvider> MakeMirrorEnv() {
    return std::make_unique<MirrorEnv>();
}

} // namespace zeromesh::mirror
