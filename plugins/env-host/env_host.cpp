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

std::string_view SandboxPathEnv() { return kSandboxPath; }

bool IsExecutableFile(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && ::access(p.c_str(), X_OK) == 0;
}

bool OnPath(const std::string& cmd, std::string_view path_env) {
    size_t start = 0;
    while (start <= path_env.size()) {
        size_t end = path_env.find(':', start);
        if (end == std::string_view::npos) end = path_env.size();
        std::string_view dir = path_env.substr(start, end - start);
        // POSIX: an empty PATH entry means the current directory, which on the
        // donor is not the task's input tree, so it is never a match.
        if (!dir.empty() && IsExecutableFile(fs::path(dir) / cmd)) return true;
        start = end + 1;
    }
    return false;
}

class HostEnv final : public plugin::IEnvProvider {
public:
    explicit HostEnv(std::string path_env) : path_env_(std::move(path_env)) {}

    std::string_view Name() const override { return "host"; }

    plugin::Availability Check(const plugin::EnvParams& params) override {
        if (params.argv.empty() || params.argv[0].empty()) {
            return {.ok = false, .reason = "empty argv"};
        }
        const std::string& cmd = params.argv[0];
        if (cmd.find('/') != std::string::npos) {
            // A relative path lives in the input tree and is checked when it runs.
            if (cmd.front() != '/') return {.ok = true, .reason = {}};
            // An absolute path is fine when it is an executable inside a directory we mount.
            for (const char* root :
                 {"/usr/", "/opt/", "/bin/", "/sbin/", "/lib/", "/lib32/", "/lib64/"}) {
                if (cmd.rfind(root, 0) == 0 && IsExecutableFile(cmd)) return {.ok = true, .reason = {}};
            }
        } else if (OnPath(cmd, path_env_)) {
            return {.ok = true, .reason = {}};
        }
        return {.ok = false, .reason = "command '" + cmd + "' not found on this node"};
    }

    plugin::Result<plugin::SandboxEnv> Prepare(const plugin::TaskContext&) override {
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
        // Host /etc/mtab is usually a symlink to ../proc/self/mounts — recreate that
        // inside the sandbox (bwrap already mounts --proc) instead of binding the host link.
        env.symlinks.push_back({"/proc/self/mounts", "/etc/mtab"});
        for (const char* p : {"/etc/alternatives", "/etc/ld.so.cache", "/etc/ssl", "/etc/passwd",
                              "/etc/group", "/etc/nsswitch.conf", "/etc/resolv.conf"}) {
            if (fs::exists(p, ec)) env.binds.push_back({p, p, true});
        }
        env.env.emplace_back("PATH", std::string(SandboxPathEnv()));
        return {.value = std::move(env), .error = {}};
    }

    void Release(const plugin::TaskContext&) override {}

private:
    std::string path_env_;
};

} // namespace

std::string DefaultPathEnv() {
    return std::string(kSandboxPath);
}

std::unique_ptr<plugin::IEnvProvider> Make(std::string path_env) {
    return std::make_unique<HostEnv>(std::move(path_env));
}

} // namespace zeromesh::envhost
