#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace zeromesh::executor {

/**
 * @brief Direct Linux Landlock LSM interface for unprivileged kernel filesystem confinement.
 * Restricts subprocess filesystem privileges even if bwrap or chroot boundaries fail.
 */
class LandlockSandbox {
public:
    LandlockSandbox() = default;

    /**
     * @brief Checks whether the running Linux kernel supports Landlock (kernel >= 5.13).
     */
    [[nodiscard]] static bool IsSupported();

    /**
     * @brief Adds a filesystem path allowed for read-only access (e.g. system compilers, input headers).
     */
    LandlockSandbox& AllowReadOnly(const std::filesystem::path& path);

    /**
     * @brief Adds a filesystem path allowed for read-write access (e.g. sandbox build output dir).
     */
    LandlockSandbox& AllowReadWrite(const std::filesystem::path& path);

    /**
     * @brief Applies the Landlock ruleset to the calling process via prctl(PR_SET_NO_NEW_PRIVS)
     * and landlock_restrict_self(). This operation is irreversible for the process and its children.
     */
    bool RestrictCurrentProcess();

private:
    std::vector<std::filesystem::path> ro_paths_;
    std::vector<std::filesystem::path> rw_paths_;
};

} // namespace zeromesh::executor
