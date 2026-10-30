#pragma once

// Freeze-first departure (v2 blueprint, claim 3). When a laptop is about to leave (lid
// closed, suspend, unplug, low battery, the owner coming back) its running tasks are not
// killed at once: the ones expected to finish inside a short grace window keep running, the
// rest are frozen with cgroup.freeze for a time-to-live, and only killed (so the requester
// re-dispatches them) if the donor has not come back by then.
//
// This header holds the pieces that need no running node: the departure reason as carried in
// a drain notice, the runtime history that predicts how long a task has left, the keep/freeze
// plan, and the signal sources (lid state, logind's PrepareForSleep).

#include "zeromesh/v1/common.pb.h"
#include "zeromesh/v1/node.pb.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

namespace zeromesh::node {

// ---- the departure reason inside a drain notice ---------------------------------------------

[[nodiscard]] const char* DepartureName(v1::DepartureReason code);

// "lid [freeze-first ttl_ms=120000] closed" -- a drain reason that also tells the requester the
// donor is freezing, not killing. `detail` is free text appended after the marker. With
// ttl_ms == 0 the result is just the name (and detail): a plain kill-first drain.
[[nodiscard]] std::string FormatDepartureReason(v1::DepartureReason code, uint32_t ttl_ms, std::string_view detail = {});

struct ParsedDeparture {
    v1::DepartureReason code{v1::DEPARTURE_UNSPECIFIED};
    uint32_t ttl_ms{0};
};
// nullopt unless `reason` carries the freeze-first marker with a non-zero ttl.
[[nodiscard]] std::optional<ParsedDeparture> ParseDepartureReason(std::string_view reason);

// ---- how long a task usually runs -----------------------------------------------------------

// Remembers, per command shape (argv, env, environment provider), a smoothed duration of
// past successful runs, so a departing donor can tell which running tasks are about to
// finish. With a file, every Record is appended there and the file is replayed on
// construction, so the history survives a restart.
class RuntimeHistory {
public:
    explicit RuntimeHistory(std::filesystem::path file = {});

    void Record(const v1::TaskSpec& spec, int64_t duration_ms);
    // nullopt for a command shape never seen.
    [[nodiscard]] std::optional<int64_t> Expected(const v1::TaskSpec& spec) const;

private:
    static std::string Key(const v1::TaskSpec& spec);
    void RecordLocked(const std::string& key, int64_t duration_ms);

    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::map<std::string, int64_t> expected_ms_;
};

// ---- the plan -------------------------------------------------------------------------------

struct RunningTask {
    std::string id;
    int64_t elapsed_ms{0};
    std::optional<int64_t> expected_ms; // nullopt: no history, so unknown
    bool preemptible{true};
};

struct DeparturePlan {
    std::vector<std::string> keep;   // run on through the grace window
    std::vector<std::string> freeze; // pause for the ttl
};

// A task is kept when it is not preemptible, or when its expected remaining time fits the
// grace window. A task past its estimate counts as about to finish. No estimate: freeze.
[[nodiscard]] DeparturePlan PlanDeparture(const std::vector<RunningTask>& tasks, std::chrono::milliseconds grace);

// ---- signals --------------------------------------------------------------------------------

struct SignalState {
    bool on_ac{true};
    bool lid_closed{false};
    bool sleep_pending{false}; // logind announced PrepareForSleep(true)
};

struct DepartureEvent {
    bool active{false}; // true: a departure begins; false: the donor is back
    v1::DepartureReason reason{v1::DEPARTURE_UNSPECIFIED};
};

// Turns successive SignalStates into departure begin/end events, one per edge. Suspend
// outranks lid, lid outranks unplug. Battery level and the owner's return are not signals
// here: DrainController already tracks them.
class DepartureWatcher {
public:
    explicit DepartureWatcher(bool depart_on_lid = true, bool depart_on_unplug = false);
    std::optional<DepartureEvent> Update(const SignalState& s);

private:
    bool depart_on_lid_;
    bool depart_on_unplug_;
    bool active_{false};
    v1::DepartureReason reason_{v1::DEPARTURE_UNSPECIFIED};
};

// <sysroot>/proc/acpi/button/lid/*/state: true when it says "closed", nullopt when this
// machine has no lid switch.
[[nodiscard]] std::optional<bool> ReadLidClosed(const std::filesystem::path& sysroot = "/");

// One line of `busctl --json=short monitor` output: the boolean argument of a login1
// PrepareForSleep signal, nullopt for any other line.
[[nodiscard]] std::optional<bool> ParsePrepareForSleep(std::string_view line);

// Watches logind for PrepareForSleep by running a monitor command, and holds a delay
// inhibitor so the system waits (up to InhibitDelayMaxSec) while the callback freezes work.
// The inhibitor is released once the callback for `true` returns and taken again on `false`.
//
// Best effort: when busctl or systemd-inhibit is missing, or there is no system bus, the
// monitor simply never fires. Not verified against a live logind in the unit tests; those
// drive it with a stand-in command.
class LogindMonitor {
public:
    struct Config {
        std::string monitor_cmd;
        std::string inhibit_cmd; // empty: no delay inhibitor
    };
    using Callback = std::function<void(bool preparing_for_sleep)>;

    // The real commands, or empty ones when the tools are not installed.
    [[nodiscard]] static Config DefaultConfig();
    [[nodiscard]] static bool Available();

    LogindMonitor(Config cfg, Callback cb);
    ~LogindMonitor();
    LogindMonitor(const LogindMonitor&) = delete;
    LogindMonitor& operator=(const LogindMonitor&) = delete;

    void Start();
    void Stop();

private:
    void Loop(std::stop_token st, int fd);
    void TakeInhibitor();
    void ReleaseInhibitor();

    Config cfg_;
    Callback cb_;
    std::mutex mu_;
    int monitor_pid_{-1};
    int inhibitor_pid_{-1};
    std::jthread thread_;
};

} // namespace zeromesh::node
