#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "node/donor_policy.hpp"

#include <sstream>

using zeromesh::node::DonorPolicy;

namespace {

zeromesh::net::Telemetry Laptop(int battery, bool on_ac) {
    zeromesh::net::Telemetry t;
    t.cores_total = 8;
    t.cores_free = 8;
    t.mem_available = 1ull << 30;
    t.battery_pct = battery;
    t.on_ac = on_ac;
    t.arch = "x86_64";
    return t;
}

zeromesh::v1::TaskSpec Echo() {
    zeromesh::v1::TaskSpec s;
    s.add_argv("echo");
    return s;
}

} // namespace

TEST(DonorPolicyDefaults) {
    DonorPolicy p;
    ASSERT_EQ(p.allow_on_battery_above, 50);
    ASSERT_EQ(p.CoreLimit(8), 7);
    ASSERT_FALSE(p.allow_network);
    ASSERT_EQ(p.drain_below_battery, 20);

    auto why = p.Refuses(Laptop(30, false), Echo(), 0);
    ASSERT_TRUE(why.has_value());
    ASSERT_TRUE(why->find("battery") != std::string::npos);
    ASSERT_FALSE(p.Refuses(Laptop(30, true), Echo(), 0).has_value());
    ASSERT_FALSE(p.Refuses(Laptop(80, false), Echo(), 0).has_value());
    ASSERT_TRUE(p.ShouldDrain(Laptop(10, false)));
    ASSERT_FALSE(p.ShouldDrain(Laptop(10, true)));

    // At most cores_total - 1 cores, counting what is already promised.
    ASSERT_FALSE(p.Refuses(Laptop(-1, true), Echo(), 6).has_value());
    ASSERT_TRUE(p.Refuses(Laptop(-1, true), Echo(), 7).has_value());
    auto big = Echo();
    big.mutable_resources()->set_cores(8);
    ASSERT_TRUE(p.Refuses(Laptop(-1, true), big, 0).has_value());
}

TEST(DonorPolicyNetwork) {
    auto spec = Echo();
    spec.mutable_policy()->set_allow_network(true);
    DonorPolicy p;
    auto why = p.Refuses(Laptop(-1, true), spec, 0);
    ASSERT_TRUE(why.has_value());
    ASSERT_TRUE(why->find("network") != std::string::npos);
    p.allow_network = true;
    ASSERT_FALSE(p.Refuses(Laptop(-1, true), spec, 0).has_value());
}

TEST(DonorPolicyParse) {
    zeromesh::testing::TempDir dir;
    ASSERT_EQ(DonorPolicy::Load(dir.path()).allow_on_battery_above, 50);  // no file: defaults

    zeromesh::testing::WriteFile(dir / "donor.toml",
                                 "# donate generously\n"
                                 "allow_on_battery_above = 80   # trailing comment\n"
                                 "\n"
                                 "max_cores=3\n"
                                 "allow_network = true\n");
    DonorPolicy p = DonorPolicy::Load(dir.path());
    ASSERT_EQ(p.allow_on_battery_above, 80);
    ASSERT_EQ(p.max_cores, 3);
    ASSERT_EQ(p.CoreLimit(8), 3);
    ASSERT_TRUE(p.allow_network);
    ASSERT_EQ(p.drain_below_battery, 20);

    std::ostringstream warn;
    DonorPolicy q = DonorPolicy::Parse("colour = blue\ndrain_below_battery = lots\nnonsense\n", warn);
    ASSERT_EQ(q.drain_below_battery, 20);
    const std::string w = warn.str();
    ASSERT_TRUE(w.find("donor.toml:1: unknown key 'colour'") != std::string::npos);
    ASSERT_TRUE(w.find("donor.toml:2: bad value") != std::string::npos);
    ASSERT_TRUE(w.find("donor.toml:3:") != std::string::npos);
}
