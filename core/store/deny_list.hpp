#pragma once

// Paths that never leave this machine: keys, credentials, shell history, browser profiles and
// ZeroMesh's own state. Applied to every workspace snapshot and every environment capture, so a
// secret cannot reach a donor however the adapter describes its inputs. Users add globs (one per
// line, gitignore syntax) in ~/.config/zeromesh/deny.

#include "store/ignore.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace zeromesh::store {

const std::vector<std::string>& DefaultDenyPatterns();

// ~/.config/zeromesh/deny, or empty when HOME is unset.
std::filesystem::path DefaultDenyFile();

// The default patterns plus the lines of `user_file` (when it exists).
IgnoreRules DenyRules(const std::filesystem::path& user_file = DefaultDenyFile());

} // namespace zeromesh::store
