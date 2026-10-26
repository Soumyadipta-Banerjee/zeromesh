#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace zeromesh::net {

// Point-in-time resource snapshot of this node, announced over discovery.
struct Telemetry {
    int cores_total{0};
    int cores_free{0};
    uint64_t mem_total{0};      // bytes
    uint64_t mem_available{0};  // bytes
    double cpu_load_pct{0.0};   // 0..100
    int battery_pct{-1};        // -1 when there is no battery
    bool on_ac{true};
    std::string arch{};
};

// Reads telemetry from procfs/sysfs under `sysroot` (tests point it at a fake tree).
// CPU load is the delta between consecutive /proc/stat samples, so the first Sample()
// reports 0. Not thread-safe: callers serialize calls to Sample().
class TelemetrySampler {
public:
    explicit TelemetrySampler(std::filesystem::path sysroot = "/");

    Telemetry Sample();

private:
    std::filesystem::path sysroot_;
    bool have_prev_{false};
    uint64_t prev_total_{0};
    uint64_t prev_idle_{0};
};

} // namespace zeromesh::net
