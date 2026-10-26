#include "scheduler/heuristics.hpp"

#include <algorithm>
#include <string_view>

namespace zeromesh::scheduler {

bool SchedulerHeuristics::IsCriticalPath(const std::vector<std::string>& command_args) {
    if (command_args.empty()) return false;

    std::string_view binary = command_args[0];
    auto slash = binary.find_last_of('/');
    if (slash != std::string_view::npos) {
        binary = binary.substr(slash + 1);
    }

    // Linkers and fat archives are almost always serial critical path bottlenecks
    if (binary == "ld" || binary == "lld" || binary == "gold" || binary == "ar" || binary == "ranlib") {
        return true;
    }

    // Direct compiler invocation performing final link step (no -c flag)
    if (binary == "gcc" || binary == "g++" || binary == "clang" || binary == "clang++") {
        bool has_compile_only = false;
        for (const auto& arg : command_args) {
            if (arg == "-c" || arg == "-E" || arg == "-S") {
                has_compile_only = true;
                break;
            }
        }
        if (!has_compile_only) {
            return true; // linking step
        }
    }

    return false;
}

bool SchedulerHeuristics::ShouldOffload(
    int64_t missing_input_bytes,
    double remote_cpu_idle_pct,
    double local_cpu_load_pct,
    std::chrono::milliseconds peer_latency
) {
    // If local CPU is almost idle (< 30%), keeping compilation local avoids network overhead
    if (local_cpu_load_pct < 30.0 && missing_input_bytes > 5 * 1024 * 1024) {
        return false;
    }

    // Remote node must have substantial idle capacity
    if (remote_cpu_idle_pct < 40.0) {
        return false;
    }

    // High network transfer latency check: if transfer takes > 1.5s, stay local
    double transfer_time_ms = EstimateTransferDurationMs(missing_input_bytes) + (peer_latency.count() * 2);
    if (transfer_time_ms > 1500.0 && local_cpu_load_pct < 85.0) {
        return false;
    }

    return true;
}

double SchedulerHeuristics::EstimateTransferDurationMs(int64_t bytes, double bandwidth_mbps) {
    if (bytes <= 0) return 0.0;
    double bytes_per_sec = (bandwidth_mbps * 1000.0 * 1000.0) / 8.0;
    return (static_cast<double>(bytes) / bytes_per_sec) * 1000.0;
}

} // namespace zeromesh::scheduler
