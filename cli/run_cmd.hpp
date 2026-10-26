#pragma once

#include "cli/client.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::cli {

struct RunOptions {
    std::vector<std::string> outputs;  // -o PATH (repeatable)
    bool remote_only{false};
    bool local_only{false};
    int cores{0};
    int64_t mem_bytes{0};
    std::string env{"host"};
    std::vector<std::string> mirror_paths;  // --mirror (repeatable); required for --env mirror
    bool force_large{false};
    // When set, one task per filesystem match of this glob (relative to cwd).
    std::optional<std::string> each_glob;
    std::vector<std::string> argv;  // after "--"
};

// Parses the arguments after "run". nullopt with *error set on a usage error.
std::optional<RunOptions> ParseRunArgs(const std::vector<std::string>& args, std::string* error);

// "512M", "2G", "1.5GiB", "1048576" -> bytes (binary units). nullopt when malformed.
std::optional<int64_t> ParseSize(const std::string& s);

// Expands a glob relative to `cwd`. Matches are paths relative to cwd, sorted.
// Returns false with *error set when the pattern is empty, invalid, or matches nothing.
bool ExpandEachGlob(const std::string& pattern, const std::filesystem::path& cwd,
                    std::vector<std::string>* matches, std::string* error);

// Replaces every "{}" in `s` with `replacement`.
std::string ReplaceBraces(std::string_view s, std::string_view replacement);

// zm run: upload the current directory, run one task (or one per --each match), stream
// logs, write outputs back and return an exit code (non-zero if any task failed).
int RunCommand(Client& client, const RunOptions& opts);

extern const char* const kRunUsage;

} // namespace zeromesh::cli
