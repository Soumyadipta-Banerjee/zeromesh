#pragma once

// Donor policy (spec §12): when this node accepts work from team peers. The
// defaults are safe: donate on AC, or on battery above 50%; use at most
// cores_total - 1 cores and 75% of memory (leaving 512 MiB); no network for tasks; drain below
// 20% battery.
// Sticky drain (M4): once a drain condition trips, stay drained until it has
// been clear for drain_hold_s (default 300).

#include "net/telemetry.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace zeromesh::node {

// Why a donor refuses a task, in terms the requester can act on without reading the prose.
enum class RefusalKind {
    Busy,        // passing: cores or memory taken, battery low; retry after retry_after_ms
    Unsupported, // never for this task on this node (needs network, other arch, more cores than donated)
};

struct Refusal {
    RefusalKind kind{RefusalKind::Busy};
    std::string reason;
    int retry_after_ms{0}; // Busy only
};

// What a batch asks of a donor while it runs.
struct Demand {
    int cores{1};
    int64_t mem_bytes{0}; // reserved, to pack it beside other batches (at most the whole budget)
    int64_t mem_need{0};  // known to need at least
    int64_t mem_limit{0}; // a hard limit it may use up to
    bool alone{false};    // must have the node's memory to itself
    int gpus{0};
    bool network{false};
    std::string arch;
};

// What a batch holds of a node's memory budget: its reservation, but never more than the whole
// budget (a batch reserving more simply runs alone), and all of it when it must run alone. With no
// budget (0), what it reserved.
int64_t HeldMemory(int64_t reserved, bool alone, int64_t budget);

struct DonorPolicy {
    int allow_on_battery_above{50}; // percent; on battery, accept only above this
    int max_cores{-1};              // -1 = cores_total - 1
    // The share of this machine's memory all work run here may use together, the owner's own runs
    // included; at least kMemoryKept stays for the system, zmd and the owner. The kernel holds work
    // to it (exec/cgroup.hpp): an estimate that is too small ends in an OOM kill of that batch,
    // never in a machine out of memory.
    int max_memory_pct{75};
    static constexpr uint64_t kMemoryKept = 512ull << 20;
    bool allow_network{false};      // batches may ask for network only if this is true
    bool allow_gpu{true};           // batches may use this node's GPUs
    int drain_below_battery{20};    // percent; on battery below this, refuse and drain
    // Off by default: keyboard IRQs made demo nodes announce "draining" and
    // vanish from peer lists. Re-enable via ~/.config/zeromesh/donor.toml when wanted.
    bool drain_on_user_activity{false};
    int drain_hold_s{300}; // seconds the drain condition must stay clear

    // Owner comfort governor (claim 2): tolerated owner stall per second of wall time (ms),
    // the highest ladder step it may take (4 = freeze), and seconds of calm before a step up.
    int comfort_target_ms{50};
    int comfort_max_step{4};
    int comfort_hold_s{10};

    // Tail hedging (claim 3): the most simultaneous copies of one task, original included.
    // 1 turns hedging off.
    int max_copies{2};
    // Freeze-first departure (claim 3): tasks expected to finish within drain_grace_s keep
    // running when this node leaves; the rest are frozen for up to freeze_ttl_s before being
    // killed. depart_on_lid / depart_on_unplug choose which signals start a departure.
    int drain_grace_s{20};
    int freeze_ttl_s{120};
    bool depart_on_lid{true};
    bool depart_on_unplug{false};

    // Reads <config_dir>/donor.toml: `key = value` lines, '#' starts a comment. A missing
    // file gives the defaults. Unknown keys and bad values are reported on stderr and ignored.
    static DonorPolicy Load(const std::filesystem::path& config_dir);
    // The parser behind Load; warnings go to `warn` (prefixed with `origin:line:`).
    static DonorPolicy Parse(std::string_view text, std::ostream& warn, std::string_view origin = "donor.toml");

    // The number of cores this node donates in total.
    [[nodiscard]] int CoreLimit(int cores_total) const;
    // max_memory_pct of mem_total, leaving kMemoryKept (half of a tiny machine), in bytes; 0 when
    // mem_total is unknown.
    [[nodiscard]] uint64_t MemoryBudget(uint64_t mem_total) const;

    // True when a drain condition is active right now (battery and/or user activity).
    // Does not apply hysteresis; see DrainController for sticky drain.
    [[nodiscard]] bool DrainCondition(const net::Telemetry& t) const;

    // True when the node should stop accepting work and drain (battery only; kept for
    // callers that only care about the battery threshold).
    [[nodiscard]] bool ShouldDrain(const net::Telemetry& t) const;

    // nullopt when a batch with `demand` is acceptable right now given the node's telemetry, the
    // cores, memory and GPUs already promised to other batches; otherwise why not, and for how long.
    [[nodiscard]] std::optional<Refusal> Evaluate(const net::Telemetry& t, const Demand& demand, int cores_in_use,
                                                  int64_t mem_promised = 0, int gpus_in_use = 0) const;
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
