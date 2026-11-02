#include "plugins/env-host/env_host.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>

namespace zeromesh::envhost {

namespace {

namespace fs = std::filesystem;

constexpr const char* kSandboxPath = "/usr/local/bin:/usr/bin:/bin";

std::string_view SandboxPathEnv() {
    return kSandboxPath;
}

bool IsExecutableFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
}

class HostEnv final : public plugin::IEnvProvider {
public:
    std::string_view Name() const override { return "host"; }

    // Programs are checked by the core against the batch's Env before an offer is accepted.
    plugin::Availability Check(const plugin::EnvParams&) override { return {.ok = true, .reason = {}}; }

    plugin::Result<plugin::SandboxEnv> Prepare(const plugin::BatchContext&) override {
        plugin::SandboxEnv env;
        env.binds.push_back({"/usr", "/usr", true});
        std::error_code ec;
        // AppImage / vendor trees (Blender under /opt, Intel oneAPI libsycl, etc.).
        if (fs::exists("/opt", ec)) env.binds.push_back({"/opt", "/opt", true});
        for (const char* p : {"/bin", "/sbin", "/lib", "/lib32", "/lib64"}) {
            if (fs::is_symlink(p, ec)) {
                fs::path target = fs::read_symlink(p, ec);
                if (!ec) env.symlinks.push_back({target.string(), p});
            } else if (fs::is_directory(p, ec)) {
                env.binds.push_back({p, p, true});
            }
        }
        // Whole /etc: Blender's setmntent("/etc/mtab") needs the host mtab (usually a
        // symlink to ../proc/self/mounts). Piecemeal binds leave debian Blender exiting 1
        // with only "could not get a list of mounted file-systems".
        if (fs::exists("/etc", ec)) env.binds.push_back({"/etc", "/etc", true});
        env.env.emplace_back("PATH", std::string(SandboxPathEnv()));
        return {.value = std::move(env), .error = {}};
    }

    void Release(const plugin::BatchContext&) override {}

};

} // namespace

std::string DefaultPathEnv() {
    return std::string(kSandboxPath);
}

std::optional<std::string> Resolve(const std::string& program) {
    if (program.empty()) return std::nullopt;
    if (program.front() == '/') {
        // Only executables inside a directory the sandbox mounts can run there.
        for (const char* root : {"/usr/", "/opt/", "/bin/", "/sbin/", "/lib/", "/lib32/", "/lib64/"}) {
            if (program.rfind(root, 0) == 0 && IsExecutableFile(program)) return program;
        }
        return std::nullopt;
    }
    if (program.find('/') != std::string::npos) return std::nullopt; // relative: lives in /work
    std::string_view path_env = SandboxPathEnv();
    size_t start = 0;
    while (start <= path_env.size()) {
        size_t end = path_env.find(':', start);
        if (end == std::string_view::npos) end = path_env.size();
        const std::string_view dir = path_env.substr(start, end - start);
        if (!dir.empty() && IsExecutableFile(fs::path(dir) / program)) return (fs::path(dir) / program).string();
        start = end + 1;
    }
    return std::nullopt;
}

std::unique_ptr<plugin::IEnvProvider> Make() {
    return std::make_unique<HostEnv>();
}

} // namespace zeromesh::envhost
