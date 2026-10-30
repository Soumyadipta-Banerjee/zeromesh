#pragma once

// Donor policy (spec §12): when this node accepts work from team peers. The
// defaults are safe: donate on AC, or on battery above 50%; use at most
// cores_total - 1 cores; no network for tasks; drain below 20% battery.
// Sticky drain (M4): once a drain condition trips, stay drained until it has
// been clear for drain_hold_s (default 300).

#include "net/telemetry.hpp"

#include "zeromesh/v1/common.pb.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace zeromesh::node {

struct DonorPolicy {
    int allow_on_battery_above{50}; // percent; on battery, accept only above this
    int max_cores{-1};              // -1 = cores_total - 1
    bool allow_network{false};      // tasks may ask for network only if this is true
    int drain_below_battery{20};    // percent; on battery below this, refuse and drain
    // Off by default: keyboard IRQs made demo nodes announce "draining" and
    // vanish from peer lists. Re-enable via ~/.config/zeromesh/donor.toml when wanted.
    bool drain_on_user_activity{false};
    int drain_hold_s{300}; // seconds the drain condition must stay clear

    // Reads <config_dir>/donor.toml: `key = value` lines, '#' starts a comment. A missing
    // file gives the defaults. Unknown keys and bad values are reported on stderr and ignored.
    static DonorPolicy Load(const std::filesystem::path& config_dir);
    // The parser behind Load; warnings go to `warn` (prefixed with `origin:line:`).
    static DonorPolicy Parse(std::string_view text, std::ostream& warn, std::string_view origin = "donor.toml");

    // The number of cores this node donates in total.
    [[nodiscard]] int CoreLimit(int cores_total) const;

    // True when a drain condition is active right now (battery and/or user activity).
    // Does not apply hysteresis; see DrainController for sticky drain.
    [[nodiscard]] bool DrainCondition(const net::Telemetry& t) const;

    // True when the node should stop accepting work and drain (battery only; kept for
    // callers that only care about the battery threshold).
    [[nodiscard]] bool ShouldDrain(const net::Telemetry& t) const;

    // nullopt when a task with `spec` is acceptable right now given the node's telemetry and
    // the cores already promised to other tasks; otherwise a human-readable reason.
    [[nodiscard]] std::optional<std::string> Refuses(const net::Telemetry& t, const v1::TaskSpec& spec,
                                                     int cores_in_use) const;
};

// Sticky drain state machine. Call Update with each telemetry sample; it reports whether
// the node should announce and refuse work as Draining, and when a transition happened.
class DrainController {
public:
    using Clock = std::chrono::steady_clock;

    explicit DrainController(DonorPolicy policy = {});

    void SetPolicy(DonorPolicy policy);
    [[nodiscard]] const DonorPolicy& policy() const { return policy_; }

    struct TickResult {
        bool draining{false};
        std::string reason;  // non-empty while draining
        bool entered{false}; // transitioned into draining this call
        bool left{false};    // transitioned out of draining this call
    };

    // Applies one sample. `now` is injectable for tests.
    TickResult Tick(const net::Telemetry& t, Clock::time_point now = Clock::now());

    [[nodiscard]] bool draining() const { return draining_; }
    [[nodiscard]] const std::string& reason() const { return reason_; }

private:
    DonorPolicy policy_;
    bool draining_{false};
    std::string reason_;
    std::optional<Clock::time_point> clear_since_;
};

} // namespace zeromesh::node
