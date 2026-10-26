#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace zeromesh::scheduler {

/**
 * @brief Heuristics and cost-model engine for locality-aware compilation dispatch.
 */
class SchedulerHeuristics {
public:
    /**
     * @brief Detects whether an action is part of the build's critical path (e.g. linking or fat archives).
     * Critical path actions should be pinned locally to prevent network transfer bottlenecks.
     */
    static bool IsCriticalPath(const std::vector<std::string>& command_args);

    /**
     * @brief Evaluates whether offloading to a remote donor will beat local execution,
     * taking into account network transfer time of missing CAS bytes versus local CPU saturation.
     */
    static bool ShouldOffload(
        int64_t missing_input_bytes,
        double remote_cpu_idle_pct,
        double local_cpu_load_pct,
        std::chrono::milliseconds peer_latency
    );

    /**
     * @brief Computes estimated network transfer duration in milliseconds.
     */
    static double EstimateTransferDurationMs(int64_t bytes, double bandwidth_mbps = 200.0);
};

} // namespace zeromesh::scheduler
