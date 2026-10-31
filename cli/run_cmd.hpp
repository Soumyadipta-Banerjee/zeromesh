#pragma once

#include "cli/client.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::cli {

struct RunOptions {
    std::vector<std::string> outputs; // -o PATH (repeatable); none: every file the command changes
    bool remote_only{false};
    bool local_only{false};
    int cores{0};
    int64_t mem_bytes{0};
    int gpus{0};
    std::string env{"host"};             // host | capture | sha256:<tree digest>
    std::vector<std::string> capture;    // --capture PATH: added to a captured environment
    std::string needs_version;           // --needs-version CONSTRAINT for the command's program
    bool force_large{false};
    bool keep_going{false};
    bool no_cache{false};
    std::string report;                  // --report PATH
    std::optional<std::string> each_glob; // one item per match of this glob (relative to cwd)
    std::optional<std::string> from;     // --from run.json: a whole run, as JSON
    std::vector<std::string> argv;       // after "--"
};

// Parses the arguments after "run". nullopt with *error set on a usage error.
std::optional<RunOptions> ParseRunArgs(const std::vector<std::string>& args, std::string* error);

// "512M", "2G", "1.5GiB", "1048576" -> bytes (binary units). nullopt when malformed.
std::optional<int64_t> ParseSize(const std::string& s);

// Expands a glob relative to `cwd`. Matches are paths relative to cwd, sorted.
// Returns false with *error set when the pattern is empty, invalid, or matches nothing.
bool ExpandEachGlob(const std::string& pattern, const std::filesystem::path& cwd, std::vector<std::string>* matches,
                    std::string* error);

// Replaces every "{}" in `s` with `replacement`.
std::string ReplaceBraces(std::string_view s, std::string_view replacement);

// A capture of `probe` (empty: none) and `paths`, as zmd needs it: the program found with this
// shell's PATH, paths made absolute, and the current directory as the probe's working directory.
// nullopt with *error when the program cannot be found.
std::optional<v1::CaptureRequest> CaptureRequestFor(const std::vector<std::string>& probe,
                                                    const std::vector<std::string>& paths, std::string* error);

// zm run: snapshot the current directory, run one item (or one per --each match), stream its
// output, write its outputs back, and return the command's exit code (non-zero if any item failed).
int RunCommand(Client& client, const RunOptions& opts);

// Follows a run until it ends. With `stream_logs`, item output is copied to stdout/stderr as it
// arrives; otherwise one line per finished item. Ctrl-C cancels the run. Returns the final results
// by item id (empty with *error when the stream broke) and the final status.
struct Watched {
    std::map<std::string, v1::ItemResult> results;
    v1::RunStatus status;
    bool finished{false};
};
Watched WatchRun(Client& client, const std::string& run_id, bool stream_logs, bool item_lines, std::string* error);

extern const char* const kRunUsage;

} // namespace zeromesh::cli
