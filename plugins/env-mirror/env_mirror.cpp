#include "plugins/env-mirror/env_mirror.hpp"
#include "store/store.hpp"

#include <filesystem>
#include <mutex>
#include <unordered_map>

namespace zeromesh::mirror {

namespace {

namespace fs = std::filesystem;

class MirrorEnv final : public plugin::IEnvProvider {
public:
    explicit MirrorEnv(std::shared_ptr<IChunkSource> fallback_source)
        : fallback_source_(std::move(fallback_source)) {}

    ~MirrorEnv() override {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [task_id, fuse_inst] : active_mounts_) {
            if (fuse_inst) {
                fuse_inst->Unmount();
            }
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

        return {.ok = true, .reason = {}};
    }

    plugin::Result<plugin::SandboxEnv> Prepare(const plugin::TaskContext& ctx) override {
        if (!MirrorFuse::IsFuseAvailable()) {
            return {.value = std::nullopt, .error = "FUSE is not available on this donor"};
        }

        fs::path base_scratch = ctx.scratch_dir.empty()
            ? (fs::temp_directory_path() / ("zm-mirror-" + ctx.task_id))
            : ctx.scratch_dir;
        fs::path mount_dir = base_scratch / "mirror";
        fs::path cache_dir = base_scratch / "mirror_cache";
        std::error_code ec;
        fs::create_directories(mount_dir, ec);
        fs::create_directories(cache_dir, ec);

        std::shared_ptr<IChunkSource> chunk_source;
        if (ctx.blob_source) {
            chunk_source = std::make_shared<BlobSourceChunkSource>(ctx.blob_source);
        } else if (fallback_source_) {
            chunk_source = fallback_source_;
        } else {
            // Default to local CAS store if available
            auto cas = std::make_shared<store::CasStore>();
            chunk_source = std::make_shared<CasChunkSource>(cas);
        }

        auto cache = std::make_shared<MirrorCache>(cache_dir, 512 * 1024 * 1024);
        auto fuse_inst = std::make_shared<MirrorFuse>(mount_dir, chunk_source, cache);

        std::string root_digest;
        auto it = ctx.env.params.find("root_digest");
        if (it != ctx.env.params.end()) {
            root_digest = it->second;
        } else {
            auto it2 = ctx.env.params.find("manifest_digest");
            if (it2 != ctx.env.params.end()) {
                root_digest = it2->second;
            }
        }

        bool mounted = false;
        if (!root_digest.empty()) {
            mounted = fuse_inst->Mount(root_digest);
        } else {
            auto empty_root = std::make_shared<VirtualNode>();
            empty_root->name = "";
            empty_root->is_directory = true;
            mounted = fuse_inst->MountWithRoot(std::move(empty_root));
        }

        if (!mounted) {
            return {.value = std::nullopt, .error = "failed to mount FUSE mirror filesystem"};
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            active_mounts_[ctx.task_id] = fuse_inst;
        }

        plugin::SandboxEnv env;
        // Bind the entire mirror tree into the sandbox read-only
        env.binds.push_back({
            .host = mount_dir.string(),
            .sandbox = mount_dir.string(),
            .read_only = true,
        });

        // Also bind to /mirror convenience path
        env.binds.push_back({
            .host = mount_dir.string(),
            .sandbox = "/mirror",
            .read_only = true,
        });

        // If the mirror exported /usr or /opt, bind them directly; otherwise fallback to donor /usr
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

        // usr-merge layout
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
        std::shared_ptr<MirrorFuse> fuse_inst;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = active_mounts_.find(ctx.task_id);
            if (it != active_mounts_.end()) {
                fuse_inst = it->second;
                active_mounts_.erase(it);
            }
        }

        if (fuse_inst) {
            fuse_inst->Unmount();
        }

        // Secondary guarantee: ensure no mount remains at mount_dir
        fs::path base_scratch = ctx.scratch_dir.empty()
            ? (fs::temp_directory_path() / ("zm-mirror-" + ctx.task_id))
            : ctx.scratch_dir;
        fs::path mount_dir = base_scratch / "mirror";
        if (MirrorFuse::IsPathMounted(mount_dir)) {
            ::system(("fusermount3 -u -z " + mount_dir.string() + " 2>/dev/null").c_str());
        }
    }

private:
    std::shared_ptr<IChunkSource> fallback_source_;
    std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<MirrorFuse>> active_mounts_;
};

} // namespace

std::unique_ptr<plugin::IEnvProvider> MakeMirrorEnv(
    std::shared_ptr<IChunkSource> fallback_chunk_source) {
    return std::make_unique<MirrorEnv>(std::move(fallback_chunk_source));
}

} // namespace zeromesh::mirror
