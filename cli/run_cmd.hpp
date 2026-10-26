#pragma once

#include "cli/client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::cli {

struct RunOptions {
    std::vector<std::string> outputs;  // -o PATH (repeatable)
    bool remote_only{false};
    bool local_only{false};
    int cores{0};
    int64_t mem_bytes{0};
    std::string env{"host"};
    bool force_large{false};
    std::vector<std::string> argv;     // after "--"
};

// Parses the arguments after "run". nullopt with *error set on a usage error.
std::optional<RunOptions> ParseRunArgs(const std::vector<std::string>& args, std::string* error);

// "512M", "2G", "1.5GiB", "1048576" -> bytes (binary units). nullopt when malformed.
std::optional<int64_t> ParseSize(const std::string& s);

// zm run: upload the current directory, run one task, stream its logs, write its outputs
// back into the current directory and return the task's exit code.
int RunCommand(Client& client, const RunOptions& opts);

extern const char* const kRunUsage;

} // namespace zeromesh::cli
