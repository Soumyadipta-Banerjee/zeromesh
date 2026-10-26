#pragma once

#include "store/ignore.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::mirror {

/**
 * @brief Default deny list patterns for env-mirror.
 *
 * Enforced on the requester side so sensitive paths (SSH keys, GPG keys,
 * .env files, browser profiles, zeromesh credentials) never enter the Merkle
 * manifest sent to donors.
 */
class DenyList {
public:
    /// Returns the default list of denied glob patterns.
    static const std::vector<std::string>& DefaultPatterns();

    /// Constructs an IgnoreRules instance with default denied patterns plus
    /// any user-configured globs from ~/.config/zeromesh/mirror_deny (or custom path).
    static store::IgnoreRules CreateIgnoreRules(const std::filesystem::path& custom_config_file = {});

    /// Checks if a relative path matches the default deny list.
    static bool IsDenied(std::string_view rel_path, bool is_dir = false);
};

} // namespace zeromesh::mirror
