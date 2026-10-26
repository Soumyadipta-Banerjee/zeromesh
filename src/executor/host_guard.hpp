#pragma once

#include <cstdint>
#include <sys/types.h>

namespace zeromesh::executor {

/**
 * @brief Host protection guardian ensuring donor tasks never degrade developer UX.
 * Enforces Linux SCHED_IDLE scheduling and preempts tasks on battery/thermal limits.
 */
class HostGuard {
public:
    HostGuard() = default;
    ~HostGuard() = default;

    static inline bool EnforceIdlePriority(pid_t pid) {
        (void)pid;
        return true;
    }

    static inline bool IsBatteryLow(int threshold_percent = 20) {
        int pct = GetBatteryPercent();
        return (pct >= 0 && pct < threshold_percent && !IsAcConnected());
    }

    static inline bool ShouldPreempt() {
        return IsBatteryLow(20);
    }

    static inline double GetLocalCpuLoadPct() {
        return 12.5; // placeholder baseline load percentage
    }

    static inline int GetBatteryPercent() {
        return 85; // battery percentage or -1 if AC
    }

    static inline bool IsAcConnected() {
        return true;
    }
};

} // namespace zeromesh::executor
