#pragma once

// Donor policy (spec §12): when this node accepts work from team peers. The
// defaults are safe: donate on AC, or on battery above 50%; use at most
// cores_total - 1 cores; no network for tasks; drain below 20% battery.

#include "net/telemetry.hpp"

#include "zeromesh/v1/common.pb.h"

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace zeromesh::node {

struct DonorPolicy {
    int allow_on_battery_above{50};  // percent; on battery, accept only above this
    int max_cores{-1};               // -1 = cores_total - 1
    bool allow_network{false};       // tasks may ask for network only if this is true
    int drain_below_battery{20};     // percent; on battery below this, refuse and drain

    // Reads <config_dir>/donor.toml: `key = value` lines, '#' starts a comment. A missing
    // file gives the defaults. Unknown keys and bad values are reported on stderr and ignored.
    static DonorPolicy Load(const std::filesystem::path& config_dir);
    // The parser behind Load; warnings go to `warn` (prefixed with `origin:line:`).
    static DonorPolicy Parse(std::string_view text, std::ostream& warn, std::string_view origin = "donor.toml");

    // The number of cores this node donates in total.
    [[nodiscard]] int CoreLimit(int cores_total) const;

    // True when the node should stop accepting work and drain (on battery, below the threshold).
    [[nodiscard]] bool ShouldDrain(const net::Telemetry& t) const;

    // nullopt when a task with `spec` is acceptable right now given the node's telemetry and
    // the cores already promised to other tasks; otherwise a human-readable reason.
    [[nodiscard]] std::optional<std::string> Refuses(const net::Telemetry& t, const v1::TaskSpec& spec,
                                                     int cores_in_use) const;
};

} // namespace zeromesh::node
