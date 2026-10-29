#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "node/donor_policy.hpp"

#include <chrono>
#include <optional>
#include <sstream>

using zeromesh::node::DonorPolicy;

namespace {

zeromesh::net::Telemetry Laptop(int battery, bool on_ac) {
    zeromesh::net::Telemetry t;
    t.cores_total = 8;
    t.cores_free = 8;
    t.work_cores = 7; // what zmd announces for it: the default policy's 7 of 8 cores
    t.mem_available = 1ull << 30;
    t.battery_pct = battery;
    t.on_ac = on_ac;
    t.arch = "x86_64";
    return t;
}

zeromesh::node::Demand Echo() {
    return zeromesh::node::Demand{};
}

// The reason of a refusal, or nullopt.
std::optional<std::string> Why(const DonorPolicy& p, const zeromesh::net::Telemetry& t, const zeromesh::node::Demand& d,
                               int cores_in_use) {
    if (auto r = p.Evaluate(t, d, cores_in_use)) return r->reason;
    return std::nullopt;
}

} // namespace

TEST(DonorPolicyDefaults) {
    DonorPolicy p;
    ASSERT_EQ(p.allow_on_battery_above, 50);
    ASSERT_EQ(p.CoreLimit(8), 7);
    ASSERT_EQ(p.CoreLimit(2), 2); // dual-core: donate both (else only one compile slot)
    ASSERT_EQ(p.CoreLimit(1), 1);
    ASSERT_FALSE(p.allow_network);
    ASSERT_EQ(p.drain_below_battery, 20);
    ASSERT_EQ(p.max_memory_pct, 75);
    ASSERT_EQ(p.MemoryBudget(8ull << 30), 6ull << 30);
    ASSERT_EQ(p.MemoryBudget(0), 0ull); // unknown machine size: no budget to hold reservations to
    // A small machine keeps 512 MiB for itself, whatever the share (75% of 1459 MiB would leave 365).
    ASSERT_EQ(p.MemoryBudget(1459ull << 20), 947ull << 20);
    ASSERT_EQ(p.MemoryBudget(256ull << 20), 128ull << 20); // a tiny one keeps half

    auto why = Why(p, Laptop(30, false), Echo(), 0);
    ASSERT_TRUE(why.has_value());
    ASSERT_TRUE(why->find("battery") != std::string::npos);
    ASSERT_FALSE(Why(p, Laptop(30, true), Echo(), 0).has_value());
    ASSERT_FALSE(Why(p, Laptop(80, false), Echo(), 0).has_value());
    ASSERT_TRUE(p.ShouldDrain(Laptop(10, false)));
    ASSERT_FALSE(p.ShouldDrain(Laptop(10, true)));

    // At most its work_cores, counting what is already promised.
    ASSERT_FALSE(Why(p, Laptop(-1, true), Echo(), 6).has_value());
    ASSERT_TRUE(Why(p, Laptop(-1, true), Echo(), 7).has_value());
    auto big = Echo();
    big.cores = 8;
    ASSERT_TRUE(Why(p, Laptop(-1, true), big, 0).has_value());
}

TEST(DonorPolicyHoldsMemoryReservationsToTheBudget) {
    DonorPolicy p;
    p.max_memory_pct = 50;
    auto t = Laptop(-1, true);
    t.mem_total = 8ull << 30;     // a 4 GiB budget
    t.mem_available = 1ull << 20; // what happens to be free this second plays no part
    auto spec = Echo();
    spec.mem_bytes = 1ll << 30;
    // 1 GiB fits when nothing is promised, and beside 3 GiB (exactly the budget), not beside more.
    ASSERT_FALSE(p.Evaluate(t, spec, 0, 0).has_value());
    ASSERT_FALSE(p.Evaluate(t, spec, 0, 3ll << 30).has_value());
    auto busy = p.Evaluate(t, spec, 0, (3ll << 30) + 1);
    ASSERT_TRUE(busy.has_value());
    ASSERT_TRUE(busy->kind == zeromesh::node::RefusalKind::Busy); // passing: the requester retries when memory frees
    ASSERT_TRUE(busy->reason.find("promised") != std::string::npos);
    // Reserving more than the whole budget is an estimate, not a need: it holds all of it (runs
    // alone), so it waits for an empty node but is not refused.
    spec.mem_bytes = 5ll << 30;
    ASSERT_FALSE(p.Evaluate(t, spec, 0, 0).has_value());
    auto wait = p.Evaluate(t, spec, 0, 1);
    ASSERT_TRUE(wait.has_value() && wait->kind == zeromesh::node::RefusalKind::Busy);
    // One that must run alone holds all of it too, however little it reserves.
    spec.mem_bytes = 1ll << 20;
    spec.alone = true;
    ASSERT_FALSE(p.Evaluate(t, spec, 0, 0).has_value());
    auto alone = p.Evaluate(t, spec, 0, 1);
    ASSERT_TRUE(alone.has_value() && alone->kind == zeromesh::node::RefusalKind::Busy);
    ASSERT_TRUE(alone->reason.find("to itself") != std::string::npos);
    // A known need, or a hard limit, above the budget never fits here: unsupported, saying what does.
    spec.alone = false;
    spec.mem_need = 5ll << 30;
    auto never = p.Evaluate(t, spec, 0, 0);
    ASSERT_TRUE(never.has_value());
    ASSERT_TRUE(never->kind == zeromesh::node::RefusalKind::Unsupported);
    ASSERT_TRUE(never->reason.find("needs at least 5120 MiB of memory; work on this node may use 4096 MiB") !=
                std::string::npos);
    spec.mem_need = 0;
    spec.mem_limit = 5ll << 30;
    auto limit = p.Evaluate(t, spec, 0, 0);
    ASSERT_TRUE(limit.has_value() && limit->kind == zeromesh::node::RefusalKind::Unsupported);
}

TEST(DonorPolicyNetwork) {
    auto spec = Echo();
    spec.network = true;
    DonorPolicy p;
    auto why = Why(p, Laptop(-1, true), spec, 0);
    ASSERT_TRUE(why.has_value());
    ASSERT_TRUE(why->find("network") != std::string::npos);
    p.allow_network = true;
    ASSERT_FALSE(Why(p, Laptop(-1, true), spec, 0).has_value());
}

TEST(DonorPolicyParse) {
    zeromesh::testing::TempDir dir;
    ASSERT_EQ(DonorPolicy::Load(dir.path()).allow_on_battery_above, 50); // no file: defaults

    zeromesh::testing::WriteFile(dir / "donor.toml", "# donate generously\n"
                                                     "allow_on_battery_above = 80   # trailing comment\n"
                                                     "\n"
                                                     "max_cores=3\n"
                                                     "max_memory_pct = 40\n"
                                                     "allow_network = true\n");
    DonorPolicy p = DonorPolicy::Load(dir.path());
    ASSERT_EQ(p.allow_on_battery_above, 80);
    ASSERT_EQ(p.max_cores, 3);
    ASSERT_EQ(p.CoreLimit(8), 3);
    ASSERT_EQ(p.max_memory_pct, 40);
    ASSERT_EQ(p.MemoryBudget(10ull << 30), 4ull << 30);
    ASSERT_TRUE(p.allow_network);
    ASSERT_EQ(p.drain_below_battery, 20);

    std::ostringstream warn;
    DonorPolicy q = DonorPolicy::Parse("colour = blue\ndrain_below_battery = lots\nnonsense\nmax_memory_pct = 0\n", warn);
    ASSERT_EQ(q.drain_below_battery, 20);
    ASSERT_EQ(q.max_memory_pct, 75); // 0 would let no work run: refused, the default kept
    const std::string w = warn.str();
    ASSERT_TRUE(w.find("donor.toml:1: unknown key 'colour'") != std::string::npos);
    ASSERT_TRUE(w.find("donor.toml:2: bad value") != std::string::npos);
    ASSERT_TRUE(w.find("donor.toml:3:") != std::string::npos);
    ASSERT_TRUE(w.find("donor.toml:4: bad value for max_memory_pct") != std::string::npos);
}

TEST(DonorPolicyParseM4Keys) {
    std::ostringstream warn;
    DonorPolicy p = DonorPolicy::Parse("drain_on_user_activity = false\n"
                                       "drain_hold_s = 60\n",
                                       warn);
    ASSERT_FALSE(p.drain_on_user_activity);
    ASSERT_EQ(p.drain_hold_s, 60);
    ASSERT_TRUE(warn.str().empty());
    auto active = Laptop(80, true);
    active.user_active = true;
    ASSERT_FALSE(p.DrainCondition(active));
    p.drain_on_user_activity = true;
    ASSERT_TRUE(p.DrainCondition(active));
}

TEST(DrainControllerHysteresis) {
    using zeromesh::node::DrainController;
    DonorPolicy p;
    p.drain_below_battery = 20;
    p.drain_hold_s = 300;
    p.drain_on_user_activity = false;
    DrainController ctl(p);

    const auto t0 = DrainController::Clock::now();
    auto low = Laptop(10, false);
    auto up = ctl.Tick(low, t0);
    ASSERT_TRUE(up.draining);
    ASSERT_TRUE(up.entered);
    ASSERT_FALSE(up.left);

    // Battery recovers; hold starts from this sample (clear_since).
    auto recovered = Laptop(80, true);
    const auto cleared_at = t0 + std::chrono::seconds(60);
    auto mid = ctl.Tick(recovered, cleared_at);
    ASSERT_TRUE(mid.draining);
    ASSERT_FALSE(mid.entered);
    ASSERT_FALSE(mid.left);

    // Still inside the hold window (299 s after clear).
    auto still = ctl.Tick(recovered, cleared_at + std::chrono::seconds(299));
    ASSERT_TRUE(still.draining);

    // Hold elapsed (300 s after clear).
    auto done = ctl.Tick(recovered, cleared_at + std::chrono::seconds(300));
    ASSERT_FALSE(done.draining);
    ASSERT_TRUE(done.left);

    // User activity with the flag on.
    p.drain_on_user_activity = true;
    p.drain_hold_s = 5;
    ctl.SetPolicy(p);
    auto active = Laptop(80, true);
    active.user_active = true;
    auto u = ctl.Tick(active, t0);
    ASSERT_TRUE(u.draining);
    ASSERT_TRUE(u.reason.find("user activity") != std::string::npos);
    active.user_active = false;
    const auto idle_at = t0 + std::chrono::seconds(1);
    ASSERT_TRUE(ctl.Tick(active, idle_at).draining);
    ASSERT_TRUE(ctl.Tick(active, idle_at + std::chrono::seconds(5)).left);
}

TEST(DonorPolicyRefusalKinds) {
    using zeromesh::node::RefusalKind;
    DonorPolicy p; // the laptop lets work hold 7 of its 8 cores
    auto spec = Echo();

    auto battery = p.Evaluate(Laptop(30, false), spec, 0);
    ASSERT_TRUE(battery.has_value());
    ASSERT_TRUE(battery->kind == RefusalKind::Busy);
    ASSERT_TRUE(battery->retry_after_ms > 0);

    auto busy = p.Evaluate(Laptop(-1, true), spec, 7);
    ASSERT_TRUE(busy.has_value());
    ASSERT_TRUE(busy->kind == RefusalKind::Busy);

    // Needing more than the donor ever donates is not "busy": waiting will not help.
    spec.cores = 8;
    auto too_big = p.Evaluate(Laptop(-1, true), spec, 0);
    ASSERT_TRUE(too_big.has_value());
    ASSERT_TRUE(too_big->kind == RefusalKind::Unsupported);

    auto net = Echo();
    net.network = true;
    auto no_net = p.Evaluate(Laptop(-1, true), net, 0);
    ASSERT_TRUE(no_net.has_value());
    ASSERT_TRUE(no_net->kind == RefusalKind::Unsupported);

    ASSERT_FALSE(p.Evaluate(Laptop(-1, true), Echo(), 0).has_value());
}

TEST(DonorPolicyGpus) {
    using zeromesh::node::RefusalKind;
    DonorPolicy p;
    auto t = Laptop(-1, true);
    auto gpu = Echo();
    gpu.gpus = 1;
    auto none = p.Evaluate(t, gpu, 0);
    ASSERT_TRUE(none.has_value() && none->kind == RefusalKind::Unsupported); // no GPU here
    t.gpus = 1;
    ASSERT_FALSE(p.Evaluate(t, gpu, 0).has_value());
    auto taken = p.Evaluate(t, gpu, 0, 0, 1);
    ASSERT_TRUE(taken.has_value() && taken->kind == RefusalKind::Busy);
    p.allow_gpu = false;
    ASSERT_TRUE(p.Evaluate(t, gpu, 0).has_value());
    std::ostringstream warn;
    ASSERT_FALSE(DonorPolicy::Parse("allow_gpu = false\n", warn).allow_gpu);
}
