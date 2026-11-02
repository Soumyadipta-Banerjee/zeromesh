#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zeromesh::net {

// Point-in-time resource snapshot of this node, announced over discovery.
struct Telemetry {
    int cores_total{0};
    int cores_free{0};
    uint64_t mem_total{0};     // bytes
    uint64_t mem_available{0}; // bytes
    double cpu_load_pct{0.0};  // 0..100
    int battery_pct{-1};       // -1 when there is no battery
    bool on_ac{true};
    bool user_active{false}; // recent keyboard/mouse IRQ activity (M4 drain)
    std::string arch{};
    int gpus{0}; // NVIDIA devices, or else DRI render nodes, under <sysroot>/dev
};

// Host device nodes a batch that asks for GPUs gets (NVIDIA control and UVM nodes included).
std::vector<std::string> GpuDevices(const std::filesystem::path& sysroot = "/");

// Reads telemetry from procfs/sysfs under `sysroot` (tests point it at a fake tree).
// CPU load is the delta between consecutive /proc/stat samples, so the first Sample()
// reports 0. User activity for M4 sticky drain:
//   1. If <sysroot>/run/zeromesh/user_active exists, it is an override ("1"/"0"/"true"/"false")
//      — used by tests.
//   2. Else sum keyboard/mouse/touchpad/USB IRQ counts from <sysroot>/proc/interrupts; the
//      user is active when the count rises by at least 8 since the previous sample
//      (filters noise). Not thread-safe: callers serialize calls to Sample().
class TelemetrySampler {
public:
    explicit TelemetrySampler(std::filesystem::path sysroot = "/");

    Telemetry Sample();

private:
    std::filesystem::path sysroot_;
    bool have_prev_{false};
    uint64_t prev_total_{0};
    uint64_t prev_idle_{0};
    bool have_irq_prev_{false};
    uint64_t prev_input_irqs_{0};
};

} // namespace zeromesh::net
