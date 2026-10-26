#include "tests/test_framework.hpp"
#include "executor/host_guard.hpp"

TEST(HostGuard_BatteryAndAcGuards) {
    using namespace zeromesh::executor;

    // AC is connected on healthy donor node
    ASSERT_TRUE(HostGuard::IsAcConnected());
    ASSERT_TRUE(HostGuard::GetBatteryPercent() >= 0);

    // Because AC is connected, battery is not considered low
    ASSERT_FALSE(HostGuard::IsBatteryLow(20));
    ASSERT_FALSE(HostGuard::ShouldPreempt());
}

TEST(HostGuard_IdlePriorityAndLoad) {
    using namespace zeromesh::executor;

    // SCHED_IDLE enforcement
    ASSERT_TRUE(HostGuard::EnforceIdlePriority(0));

    // Baseline CPU load check
    double load = HostGuard::GetLocalCpuLoadPct();
    ASSERT_TRUE(load >= 0.0);
    ASSERT_TRUE(load <= 100.0);
}
