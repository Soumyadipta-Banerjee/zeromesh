#pragma once

// Watches the memory pressure of the cgroup all work runs in (exec/cgroup.hpp). At its memory.max
// the kernel reclaims and refaults for minutes before it kills anything: on a 1.5 GB donor two
// compiles that did not fit together took 330 s instead of 7 s each, with all work stalled on memory
// a third of the time. Once all work has been stalled for a quarter of the last ten seconds (the
// kernel's "full avg10" of the work cgroup), one batch too many is running: the item that started
// last is stopped as out of memory, and the run engine retries it with a node's memory to itself.
// Retrying alone costs that item its progress; letting the work thrash costs every item running.

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace zeromesh::node {

struct WorkPressureConfig {
    std::chrono::milliseconds interval{1000};
    double stalled_pct{25.0};        // "full avg10" at which one item is stopped
    std::chrono::seconds settle{10}; // after a stop, before another: the average must show its effect
};

class WorkPressure {
public:
    // `stop_youngest` stops the item that started last; false when none runs. With an empty
    // `work_cgroup` nothing is watched (Observe still works, for tests).
    WorkPressure(std::filesystem::path work_cgroup, std::function<bool()> stop_youngest,
                 WorkPressureConfig cfg = {});
    ~WorkPressure();

    WorkPressure(const WorkPressure&) = delete;
    WorkPressure& operator=(const WorkPressure&) = delete;

    using Clock = std::chrono::steady_clock;
    // The work cgroup's "full avg10" at `now`; true when it stopped an item.
    bool Observe(double full_avg10, Clock::time_point now);

private:
    void Loop(std::stop_token st);

    std::filesystem::path pressure_file_;
    std::function<bool()> stop_youngest_;
    WorkPressureConfig cfg_;
    std::optional<Clock::time_point> last_stop_;
    std::mutex mu_;
    std::condition_variable_any cv_;
    std::jthread thread_; // last: stopped and joined first
};

} // namespace zeromesh::node
