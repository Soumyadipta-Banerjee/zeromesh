#include "executor/bwrap.hpp"

namespace zeromesh::executor {

BwrapBuilder& BwrapBuilder::SetCasMaterializedRoot(const std::filesystem::path& cas_root) {
    cas_root_ = cas_root;
    return *this;
}

BwrapBuilder& BwrapBuilder::MountEssentialPseudoDevices() {
    mount_pseudo_devices_ = true;
    return *this;
}

BwrapBuilder& BwrapBuilder::MountScratchTmpfs(const std::filesystem::path& sandbox_dest) {
    tmpfs_mounts_.push_back(sandbox_dest);
    return *this;
}

BwrapBuilder& BwrapBuilder::AddReadWriteBind(const std::filesystem::path& host_path, const std::filesystem::path& sandbox_dest) {
    rw_binds_.emplace_back(host_path, sandbox_dest);
    return *this;
}

BwrapBuilder& BwrapBuilder::SetWorkingDirectory(const std::filesystem::path& work_dir) {
    work_dir_ = work_dir;
    return *this;
}

BwrapBuilder& BwrapBuilder::AddEnv(const std::string& key, const std::string& value) {
    env_vars_.emplace_back(key, value);
    return *this;
}

std::vector<std::string> BwrapBuilder::Build(const std::vector<std::string>& command_args) const {
    std::vector<std::string> args = {"bwrap"};

    // Unprivileged namespace isolation
    args.push_back("--unshare-all");
    args.push_back("--die-with-parent");

    // Hermetic root mount from CAS if provided
    if (!cas_root_.empty()) {
        args.push_back("--ro-bind");
        args.push_back(cas_root_.string());
        args.push_back("/");
    }

    // Essential pseudo-devices
    if (mount_pseudo_devices_) {
        args.push_back("--dev");
        args.push_back("/dev");
        args.push_back("--proc");
        args.push_back("/proc");
    }

    // Memory-backed tmpfs mounts
    for (const auto& tmp : tmpfs_mounts_) {
        args.push_back("--tmpfs");
        args.push_back(tmp.string());
    }

    // Read-write mounts for workspace
    for (const auto& [host, sandbox] : rw_binds_) {
        args.push_back("--bind");
        args.push_back(host.string());
        args.push_back(sandbox.string());
    }

    // Environment variables
    for (const auto& [key, val] : env_vars_) {
        args.push_back("--setenv");
        args.push_back(key);
        args.push_back(val);
    }

    // Working directory
    args.push_back("--chdir");
    args.push_back(work_dir_.string());

    // Command invocation
    args.push_back("--");
    for (const auto& cmd : command_args) {
        args.push_back(cmd);
    }

    return args;
}

} // namespace zeromesh::executor
