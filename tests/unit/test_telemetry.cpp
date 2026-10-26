#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "net/telemetry.hpp"

#include <cmath>

using zeromesh::net::TelemetrySampler;
using zeromesh::testing::TempDir;
using zeromesh::testing::WriteFile;

namespace {

// Four cores; aggregate fields: user nice system idle iowait irq softirq steal.
std::string ProcStat(int busy, int idle) {
    std::string agg = "cpu  " + std::to_string(busy) + " 0 0 " + std::to_string(idle) + " 0 0 0 0 0 0\n";
    return agg + "cpu0 0 0 0 0 0 0 0 0 0 0\ncpu1 0 0 0 0 0 0 0 0 0 0\n"
                 "cpu2 0 0 0 0 0 0 0 0 0 0\ncpu3 0 0 0 0 0 0 0 0 0 0\nintr 0\n";
}

}  // namespace

TEST(TelemetryFakeSysroot) {
    TempDir root;
    WriteFile(root / "proc/meminfo", "MemTotal:       16000000 kB\nMemFree:  100 kB\nMemAvailable:    8000000 kB\n");
    WriteFile(root / "sys/class/power_supply/BAT0/type", "Battery\n");
    WriteFile(root / "sys/class/power_supply/BAT0/capacity", "42\n");
    WriteFile(root / "sys/class/power_supply/AC/type", "Mains\n");
    WriteFile(root / "sys/class/power_supply/AC/online", "0\n");

    TelemetrySampler sampler(root.path());
    WriteFile(root / "proc/stat", ProcStat(0, 1000));
    auto first = sampler.Sample();
    ASSERT_EQ(first.cpu_load_pct, 0.0);

    WriteFile(root / "proc/stat", ProcStat(500, 1500));  // +500 busy, +500 idle
    auto t = sampler.Sample();
    ASSERT_TRUE(std::fabs(t.cpu_load_pct - 50.0) <= 1.0);
    ASSERT_EQ(t.cores_total, 4);
    ASSERT_EQ(t.cores_free, 2);
    ASSERT_EQ(t.mem_total, 16384000000ULL);
    ASSERT_EQ(t.mem_available, 8192000000ULL);
    ASSERT_EQ(t.battery_pct, 42);
    ASSERT_FALSE(t.on_ac);
    ASSERT_FALSE(t.arch.empty());
}

TEST(TelemetryNoBatteryMeansAc) {
    TempDir root;
    WriteFile(root / "proc/stat", ProcStat(0, 100));
    WriteFile(root / "proc/meminfo", "MemTotal: 1000 kB\nMemAvailable: 500 kB\n");
    WriteFile(root / "sys/class/power_supply/AC/type", "Mains\n");
    WriteFile(root / "sys/class/power_supply/AC/online", "0\n");

    auto t = TelemetrySampler(root.path()).Sample();
    ASSERT_EQ(t.battery_pct, -1);
    ASSERT_TRUE(t.on_ac);
}
