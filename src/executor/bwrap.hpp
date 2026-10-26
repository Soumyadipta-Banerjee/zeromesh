#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace zeromesh::executor {

/**
 * @brief Bubblewrap (bwrap) argument builder and sandbox execution configuration.
 *
 * Enforces unprivileged hermetic isolation boundaries:
 *  - Do NOT blindly --ro-bind host /usr and /lib (avoids host glibc/distro mismatch)
 *  - Toolchain and runtime libraries are materialized directly from CAS digests
 *  - Mounts only essential pseudo-devices: /dev/null, /dev/zero, /dev/urandom
 *  - Mounts an ephemeral memory-backed tmpfs for scratch space (/tmp)
 *  - Sandboxed read-write workspace for input root and action outputs
 *  - Strict network unsharing (--unshare-all) to guarantee hermeticity
 */
class BwrapBuilder {
public:
    BwrapBuilder() = default;

    /**
     * @brief Mounts the CAS-materialized hermetic root containing compilers and sysroot.
     * Maps the hermetic root to / inside the sandbox.
     */
    BwrapBuilder& SetCasMaterializedRoot(const std::filesystem::path& cas_root);

    /**
     * @brief Mounts essential pseudo-devices: /dev/null, /dev/zero, /dev/urandom.
     */
    BwrapBuilder& MountEssentialPseudoDevices();

    /**
     * @brief Mounts an ephemeral memory-backed tmpfs inside the sandbox (default /tmp).
     */
    BwrapBuilder& MountScratchTmpfs(const std::filesystem::path& sandbox_dest = "/tmp");

    /**
     * @brief Adds a read-write directory bind for workspace and intermediate artifacts.
     */
    BwrapBuilder& AddReadWriteBind(const std::filesystem::path& host_path, const std::filesystem::path& sandbox_dest);

    /**
     * @brief Sets working directory inside sandbox.
     */
    BwrapBuilder& SetWorkingDirectory(const std::filesystem::path& work_dir);

    /**
     * @brief Adds environment variable key-value pair.
     */
    BwrapBuilder& AddEnv(const std::string& key, const std::string& value);

    /**
     * @brief Assembles full bwrap CLI invocation argument vector.
     */
    [[nodiscard]] std::vector<std::string> Build(const std::vector<std::string>& command_args) const;

private:
    std::filesystem::path cas_root_{};
    bool mount_pseudo_devices_{true};
    std::vector<std::filesystem::path> tmpfs_mounts_{"/tmp"};
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> rw_binds_;
    std::vector<std::pair<std::string, std::string>> env_vars_;
    std::filesystem::path work_dir_{"/workspace"};
};

} // namespace zeromesh::executor
