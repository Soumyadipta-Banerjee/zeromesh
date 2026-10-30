#include "exec/landlock.hpp"

#include <fcntl.h>
#include <linux/landlock.h>
#include <linux/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <iostream>

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#endif
#ifndef __NR_landlock_add_rule
#define __NR_landlock_add_rule 445
#endif
#ifndef __NR_landlock_restrict_self
#define __NR_landlock_restrict_self 446
#endif

#ifndef LANDLOCK_CREATE_RULESET_VERSION
#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#endif

namespace zeromesh::exec {

bool LandlockSandbox::IsSupported() {
    long abi = syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    return abi >= 1;
}

LandlockSandbox& LandlockSandbox::AllowReadOnly(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        ro_paths_.push_back(path);
    }
    return *this;
}

LandlockSandbox& LandlockSandbox::AllowReadWrite(const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        rw_paths_.push_back(path);
    }
    return *this;
}

bool LandlockSandbox::RestrictCurrentProcess() {
    if (!IsSupported()) {
        return false;
    }

    uint64_t ro_access = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

    uint64_t rw_access = ro_access | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR |
                         LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
                         LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO |
                         LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM | LANDLOCK_ACCESS_FS_REFER |
                         LANDLOCK_ACCESS_FS_TRUNCATE;

    struct landlock_ruleset_attr ruleset_attr = {};
    ruleset_attr.handled_access_fs = rw_access;

    int ruleset_fd = static_cast<int>(syscall(__NR_landlock_create_ruleset, &ruleset_attr, sizeof(ruleset_attr), 0));
    if (ruleset_fd < 0) {
        return false;
    }

    // Add read-only paths
    for (const auto& path : ro_paths_) {
        int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
        if (fd >= 0) {
            struct landlock_path_beneath_attr path_attr = {};
            path_attr.parent_fd = fd;
            path_attr.allowed_access = ro_access;
            syscall(__NR_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
            close(fd);
        }
    }

    // Add read-write paths
    for (const auto& path : rw_paths_) {
        int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
        if (fd >= 0) {
            struct landlock_path_beneath_attr path_attr = {};
            path_attr.parent_fd = fd;
            path_attr.allowed_access = rw_access;
            syscall(__NR_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
            close(fd);
        }
    }

    // Lock down process privileges
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        close(ruleset_fd);
        return false;
    }

    if (syscall(__NR_landlock_restrict_self, ruleset_fd, 0) != 0) {
        close(ruleset_fd);
        return false;
    }

    close(ruleset_fd);
    return true;
}

} // namespace zeromesh::exec
