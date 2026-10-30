#pragma once

// Owner comfort governor (v2 blueprint, claim 2). Watches the owner's resource pressure
// (Linux PSI: cpu, memory, io) and steps donated tasks down a ladder of cgroup v2 limits:
//   1 cpu.weight=1 + cpu.idle   2 + memory.high and one bounded memory.reclaim
//   3 + io.weight=1             4 cgroup.freeze (pause)
// One step per decision. Down needs `sustain_samples` consecutive stalled samples; up needs
// `hold` of calm. A cooldown separates any two steps, so alternating load cannot flap it.
//
// Pressure is sampled by polling the `total=` stall counters of /proc/pressure/* (the same
// number a PSI trigger fires on), so it needs no CAP_SYS_RESOURCE; the sampling window is
// `sample_interval`.

#include "node/donor_policy.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace zeromesh::node {

struct PsiSample {
    uint64_t some_total_us{0};
    uint64_t full_total_us{0};
};

// Parses the text of a /proc/pressure/<resource> file.
std::optional<PsiSample> ParsePsi(std::string_view text);
std::optional<PsiSample> ReadPsiFile(const std::filesystem::path& path);

// Turns a monotonically increasing stall counter (microseconds) into the fraction of wall
// time spent stalled between consecutive observations.
class StallMeter {
public:
    using Clock = std::chrono::steady_clock;
    std::optional<double> Observe(uint64_t total_us, Clock::time_point now);

private:
    bool have_prev_{false};
    uint64_t prev_total_{0};
    Clock::time_point prev_time_{};
};

struct ComfortConfig {
    double pressure_threshold{0.05}; // stalled fraction of wall time that counts as pressure
    int sustain_samples{2};
    std::chrono::seconds hold{10};
    std::chrono::seconds cooldown{2};
    int max_step{4};
    std::chrono::milliseconds sample_interval{500};

    // comfort_target_ms is the stall per second of wall time the owner tolerates.
    static ComfortConfig FromPolicy(const DonorPolicy& p);
};

class ComfortLadder {
public:
    using Clock = std::chrono::steady_clock;
    explicit ComfortLadder(ComfortConfig cfg = {});

    struct Result {
        int level{0};
        bool changed{false};
    };
    // One pressure sample (stalled fraction, 0..1) taken at `now`.
    Result Update(double pressure, Clock::time_point now);

    [[nodiscard]] int level() const { return level_; }

private:
    ComfortConfig cfg_;
    int level_{0};
    int high_count_{0};
    std::optional<Clock::time_point> calm_since_;
    std::optional<Clock::time_point> last_change_;
};

// Applies a ladder level to a set of task cgroups by writing their control files. Files
// that do not exist (a controller that was not delegated) are skipped, never created.
class CgroupActuator {
public:
    static constexpr uint64_t kMemoryHighFloor = 32ULL << 20;
    static constexpr uint64_t kReclaimBytes = 64ULL << 20;

    struct Caps {
        bool cpu{false};
        bool memory{false};
        bool io{false};
        bool freeze{false};
    };

    void Add(const std::filesystem::path& dir); // also applies the current level
    void Remove(const std::filesystem::path& dir);
    void Apply(int level);
    // Departure freeze: while held, `dir` stays frozen whatever the comfort level; releasing
    // thaws it unless the ladder itself is at the freeze step. False when cgroup.freeze cannot
    // be written (no such cgroup, or the controller is not delegated).
    bool Hold(const std::filesystem::path& dir, bool frozen);
    [[nodiscard]] int level() const;
    [[nodiscard]] Caps Capabilities() const;

private:
    void ApplyTo(const std::filesystem::path& dir, int level);

    mutable std::mutex mu_;
    int level_{0};
    std::vector<std::filesystem::path> dirs_;
    std::set<std::filesystem::path> held_;
    Caps caps_;
};

struct PsiTotals {
    uint64_t cpu_us{0};
    uint64_t mem_us{0};
    uint64_t io_us{0};
};
std::optional<PsiTotals> ReadHostPsi();

class ComfortGovernor {
public:
    using Clock = ComfortLadder::Clock;
    explicit ComfortGovernor(ComfortConfig cfg = {});
    ~ComfortGovernor();

    ComfortGovernor(const ComfortGovernor&) = delete;
    ComfortGovernor& operator=(const ComfortGovernor&) = delete;

    void AddTaskCgroup(const std::filesystem::path& dir) { actuator_.Add(dir); }
    void RemoveTaskCgroup(const std::filesystem::path& dir) { actuator_.Remove(dir); }
    // Freezes or thaws one task cgroup for a departure, independent of the comfort ladder.
    bool HoldFrozen(const std::filesystem::path& dir, bool frozen) { return actuator_.Hold(dir, frozen); }

    // One sampling step; the thread calls it, tests call it directly.
    void Tick(const PsiTotals& totals, Clock::time_point now);

    // Samples `source` every sample_interval on a background thread. No-op if PSI is absent.
    void Start(std::function<std::optional<PsiTotals>()> source = ReadHostPsi);
    void Stop();

    [[nodiscard]] int level() const;
    [[nodiscard]] CgroupActuator::Caps Capabilities() const { return actuator_.Capabilities(); }

private:
    ComfortConfig cfg_;
    mutable std::mutex mu_;
    ComfortLadder ladder_;
    StallMeter cpu_, mem_, io_;
    CgroupActuator actuator_;
    std::jthread thread_;
};

} // namespace zeromesh::node
