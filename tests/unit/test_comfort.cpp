#include "tests/unit/test_framework.hpp"

#include "node/comfort_governor.hpp"
#include "node/donor_policy.hpp"
#include "tests/unit/node_fixture.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

using namespace std::chrono_literals;
using zeromesh::node::ComfortConfig;
using zeromesh::node::ComfortGovernor;
using zeromesh::node::ComfortLadder;
using zeromesh::node::CgroupActuator;
using zeromesh::testing::TempDir;

namespace {

using Clock = ComfortLadder::Clock;

std::string Slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

void Put(const std::filesystem::path& p, const std::string& v) {
    std::ofstream(p) << v;
}

// A fake task cgroup directory: the control files a real one has, as plain files.
std::filesystem::path FakeCgroup(const std::filesystem::path& root, const std::string& name, bool with_io = true) {
    auto dir = root / name;
    std::filesystem::create_directories(dir);
    Put(dir / "cpu.weight", "100");
    Put(dir / "cpu.idle", "0");
    Put(dir / "memory.high", "max");
    Put(dir / "memory.current", std::to_string(512ULL << 20));
    Put(dir / "memory.reclaim", "");
    if (with_io) Put(dir / "io.weight", "default 100");
    Put(dir / "cgroup.freeze", "0");
    return dir;
}

ComfortConfig FastConfig() {
    ComfortConfig c;
    c.pressure_threshold = 0.05;
    c.sustain_samples = 2;
    c.hold = 10s;
    c.cooldown = 2s;
    c.max_step = 4;
    return c;
}

} // namespace

TEST(PsiParsesKernelFormat) {
    const std::string text =
        "some avg10=1.50 avg60=0.75 avg300=0.10 total=123456\n"
        "full avg10=0.25 avg60=0.00 avg300=0.00 total=789\n";
    auto s = zeromesh::node::ParsePsi(text);
    ASSERT_TRUE(s.has_value());
    ASSERT_EQ(s->some_total_us, static_cast<uint64_t>(123456));
    ASSERT_EQ(s->full_total_us, static_cast<uint64_t>(789));
}

TEST(PsiRejectsGarbage) {
    ASSERT_TRUE(!zeromesh::node::ParsePsi("hello").has_value());
    ASSERT_TRUE(!zeromesh::node::ParsePsi("").has_value());
}

TEST(PsiStallFractionIsTotalDeltaOverWallTime) {
    zeromesh::node::StallMeter m;
    const auto t0 = Clock::now();
    ASSERT_TRUE(!m.Observe(1000, t0).has_value()); // the first sample has no delta
    auto f = m.Observe(1000 + 50000, t0 + 1s);     // 50 ms stalled in one second
    ASSERT_TRUE(f.has_value());
    ASSERT_TRUE(*f > 0.049 && *f < 0.051);
    auto zero = m.Observe(1000 + 50000, t0 + 2s);
    ASSERT_TRUE(zero.has_value() && *zero == 0.0);
}

TEST(LadderIgnoresASingleSpike) {
    ComfortLadder l(FastConfig());
    const auto t = Clock::now();
    l.Update(0.5, t);
    l.Update(0.0, t + 1s);
    ASSERT_EQ(l.level(), 0);
}

TEST(LadderStepsDownOnSustainedPressureOneStepAtATime) {
    ComfortLadder l(FastConfig());
    auto t = Clock::now();
    l.Update(0.5, t);
    auto r = l.Update(0.5, t + 1s);
    ASSERT_EQ(l.level(), 1);
    ASSERT_TRUE(r.changed);
    // Cooldown: pressure persists but the next step must wait.
    l.Update(0.5, t + 2s);
    ASSERT_EQ(l.level(), 1);
    l.Update(0.5, t + 4s);
    ASSERT_EQ(l.level(), 2);
}

TEST(LadderStopsAtMaxStep) {
    auto cfg = FastConfig();
    cfg.max_step = 2;
    ComfortLadder l(cfg);
    auto t = Clock::now();
    for (int i = 0; i < 40; ++i) l.Update(0.9, t + std::chrono::seconds(i * 3));
    ASSERT_EQ(l.level(), 2);
}

TEST(LadderStepsUpOnlyAfterHoldOfCalm) {
    ComfortLadder l(FastConfig());
    auto t = Clock::now();
    l.Update(0.5, t);
    l.Update(0.5, t + 1s); // level 1
    ASSERT_EQ(l.level(), 1);
    l.Update(0.0, t + 2s);
    l.Update(0.0, t + 8s); // only 6 s of calm
    ASSERT_EQ(l.level(), 1);
    auto r = l.Update(0.0, t + 13s); // 11 s of calm
    ASSERT_EQ(l.level(), 0);
    ASSERT_TRUE(r.changed);
}

TEST(LadderDoesNotFlapOnAlternatingPressure) {
    ComfortLadder l(FastConfig());
    auto t = Clock::now();
    int changes = 0;
    for (int i = 0; i < 60; ++i) {
        auto r = l.Update(i % 2 ? 0.0 : 0.5, t + std::chrono::seconds(i));
        if (r.changed) ++changes;
    }
    ASSERT_EQ(changes, 0);
}

TEST(LadderMidPressureNeitherStepsDownNorUp) {
    // Between half the threshold and the threshold: hysteresis band.
    ComfortLadder l(FastConfig());
    auto t = Clock::now();
    l.Update(0.5, t);
    l.Update(0.5, t + 1s);
    ASSERT_EQ(l.level(), 1);
    for (int i = 2; i < 60; ++i) l.Update(0.04, t + std::chrono::seconds(i));
    ASSERT_EQ(l.level(), 1);
}

TEST(ActuatorWritesEachRungOfTheLadder) {
    TempDir tmp;
    auto cg = FakeCgroup(tmp.path(), "zm-task-a");
    CgroupActuator act;
    act.Add(cg);

    act.Apply(1);
    ASSERT_EQ(Slurp(cg / "cpu.weight"), std::string("1"));
    ASSERT_EQ(Slurp(cg / "cpu.idle"), std::string("1"));
    ASSERT_EQ(Slurp(cg / "memory.high"), std::string("max"));
    ASSERT_EQ(Slurp(cg / "cgroup.freeze"), std::string("0"));

    act.Apply(2);
    // memory.high drops to half of memory.current and one bounded reclaim is requested.
    ASSERT_EQ(Slurp(cg / "memory.high"), std::to_string(256ULL << 20));
    ASSERT_EQ(Slurp(cg / "memory.reclaim"), std::to_string(64ULL << 20));
    ASSERT_EQ(Slurp(cg / "io.weight"), std::string("default 100"));

    act.Apply(3);
    ASSERT_EQ(Slurp(cg / "io.weight"), std::string("default 1"));
    ASSERT_EQ(Slurp(cg / "cgroup.freeze"), std::string("0"));

    act.Apply(4);
    ASSERT_EQ(Slurp(cg / "cgroup.freeze"), std::string("1"));
}

TEST(ActuatorRestoresEverythingAtLevelZero) {
    TempDir tmp;
    auto cg = FakeCgroup(tmp.path(), "zm-task-a");
    CgroupActuator act;
    act.Add(cg);
    act.Apply(4);
    act.Apply(0);
    ASSERT_EQ(Slurp(cg / "cpu.weight"), std::string("100"));
    ASSERT_EQ(Slurp(cg / "cpu.idle"), std::string("0"));
    ASSERT_EQ(Slurp(cg / "memory.high"), std::string("max"));
    ASSERT_EQ(Slurp(cg / "io.weight"), std::string("default 100"));
    ASSERT_EQ(Slurp(cg / "cgroup.freeze"), std::string("0"));
}

TEST(ActuatorSkipsIoWhenControllerNotDelegated) {
    TempDir tmp;
    auto cg = FakeCgroup(tmp.path(), "zm-task-a", /*with_io=*/false);
    CgroupActuator act;
    act.Add(cg);
    act.Apply(3);
    ASSERT_EQ(Slurp(cg / "cpu.weight"), std::string("1"));
    ASSERT_TRUE(!std::filesystem::exists(cg / "io.weight")); // never created
    ASSERT_TRUE(!act.Capabilities().io);
    ASSERT_TRUE(act.Capabilities().cpu);
}

TEST(ActuatorAppliesCurrentLevelToCgroupsAddedLater) {
    TempDir tmp;
    CgroupActuator act;
    act.Apply(1);
    auto cg = FakeCgroup(tmp.path(), "zm-task-late");
    act.Add(cg);
    ASSERT_EQ(Slurp(cg / "cpu.weight"), std::string("1"));
    act.Remove(cg);
    act.Apply(0); // a removed cgroup is no longer touched
    ASSERT_EQ(Slurp(cg / "cpu.weight"), std::string("1"));
}

TEST(GovernorStepsDownTaskCgroupsWhenOwnerStalls) {
    TempDir tmp;
    auto cg = FakeCgroup(tmp.path(), "zm-task-a");
    ComfortGovernor gov(FastConfig());
    gov.AddTaskCgroup(cg);

    auto t = Clock::now();
    // Owner stalled 200 ms per second on cpu: well over the 5% threshold.
    uint64_t total = 0;
    gov.Tick({0, 0, 0}, t); // baseline
    for (int i = 1; i <= 2; ++i) {
        total += 200000;
        gov.Tick({total, 0, 0}, t + std::chrono::seconds(i));
    }
    ASSERT_EQ(gov.level(), 1);
    ASSERT_EQ(Slurp(cg / "cpu.weight"), std::string("1"));

    // Quiet again for longer than hold: back to full speed.
    for (int i = 3; i <= 20; ++i) gov.Tick({total, 0, 0}, t + std::chrono::seconds(i));
    ASSERT_EQ(gov.level(), 0);
    ASSERT_EQ(Slurp(cg / "cpu.weight"), std::string("100"));
}

TEST(GovernorTakesTheWorstOfCpuMemoryAndIo) {
    ComfortGovernor gov(FastConfig());
    auto t = Clock::now();
    gov.Tick({0, 0, 0}, t);
    gov.Tick({0, 0, 300000}, t + 1s); // only io stalls
    gov.Tick({0, 0, 600000}, t + 2s);
    ASSERT_EQ(gov.level(), 1);
}

TEST(DonorPolicyParsesComfortKeys) {
    std::ostringstream warn;
    auto p = zeromesh::node::DonorPolicy::Parse("comfort_target_ms = 80\nmax_step = 3\nhold_s = 20\n", warn);
    ASSERT_EQ(p.comfort_target_ms, 80);
    ASSERT_EQ(p.comfort_max_step, 3);
    ASSERT_EQ(p.comfort_hold_s, 20);
    ASSERT_EQ(warn.str(), std::string(""));
    auto cfg = zeromesh::node::ComfortConfig::FromPolicy(p);
    ASSERT_TRUE(cfg.pressure_threshold > 0.079 && cfg.pressure_threshold < 0.081);
    ASSERT_EQ(cfg.max_step, 3);
    ASSERT_EQ(cfg.hold, std::chrono::seconds(20));
}

TEST(HostPsiFilesAreReadable) {
    // Skips silently on kernels without PSI; on this host the files exist.
    auto s = zeromesh::node::ReadPsiFile("/proc/pressure/cpu");
    if (!std::filesystem::exists("/proc/pressure/cpu")) return;
    ASSERT_TRUE(s.has_value());
}

// ---- wiring: OfferReply.comfort_level, placer avoidance, sandbox registration, real kernel ----

#include "node/action_cache.hpp"
#include "node/convert.hpp"
#include "node/job_manager.hpp"
#include "sched/placer.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace v1 = zeromesh::v1;
using zeromesh::testing::TestNodeFull;

v1::TaskSpec EchoSpec(const std::string& name) {
    v1::TaskSpec t;
    t.set_name(name);
    t.add_argv("sh");
    t.add_argv("-c");
    t.add_argv("echo " + name);
    t.mutable_policy()->set_may_run_local(false);
    return t;
}

} // namespace

TEST(OfferReplyCarriesComfortLevelAndSqueezedDonorRefuses) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    std::atomic<int> level{0};
    b.service->SetComfortLevelProvider([&] { return level.load(); });

    auto client = a.ClientTo(b);
    std::string err;
    auto ok = client->Offer("t1", EchoSpec("t1"), {}, &err);
    ASSERT_TRUE(ok.has_value());
    ASSERT_TRUE(ok->accepted());
    ASSERT_EQ(ok->comfort_level(), static_cast<uint32_t>(0));

    level = 2; // memory step: still takes work
    auto mid = client->Offer("t2", EchoSpec("t2"), {}, &err);
    ASSERT_TRUE(mid->accepted());
    ASSERT_EQ(mid->comfort_level(), static_cast<uint32_t>(2));

    level = 3; // io step: the owner is being squeezed, so no new work
    auto no = client->Offer("t3", EchoSpec("t3"), {}, &err);
    ASSERT_TRUE(no.has_value());
    ASSERT_TRUE(!no->accepted());
    ASSERT_EQ(no->comfort_level(), static_cast<uint32_t>(3));
    ASSERT_TRUE(no->reason().find("owner busy") != std::string::npos);
}

TEST(PlacerRoutesAroundASqueezedDonor) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), busy("node-busy", key), calm("node-calm", key);
    a.Start();
    busy.Start();
    calm.Start();
    a.Admit(busy);
    a.Admit(calm);
    a.peers->Upsert(busy.AsPeer());
    a.peers->Upsert(calm.AsPeer());
    busy.service->SetComfortLevelProvider([] { return 4; });
    calm.service->SetComfortLevelProvider([] { return 0; });

    auto factory = [&a](const zeromesh::net::Peer& p) {
        return std::make_shared<zeromesh::node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port), a.id,
                                                            a.pins, p.id);
    };
    auto placer = std::make_shared<zeromesh::sched::Placer>(a.executor, a.peers, factory, a.cas, a.merkle, a.node_id,
                                                            [&a] { return a.telemetry; });
    zeromesh::node::JobManager jm(a.cas, a.merkle, std::make_shared<zeromesh::node::ActionCache>(a.cas), placer);
    v1::JobSpec job;
    job.set_name("six");
    for (int i = 0; i < 6; ++i) *job.add_tasks() = EchoSpec("c" + std::to_string(i));
    const std::string id = jm.Submit(job);
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!jm.Get(id)->done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(20ms);
    auto st = jm.Get(id);
    ASSERT_TRUE(st->done());
    for (const auto& [name, r] : st->results()) {
        ASSERT_EQ(r.executed_by(), std::string("node-calm"));
    }
}

TEST(SandboxRunnerReportsTaskCgroupsToObserver) {
    auto probe = zeromesh::exec::ProbeSandboxResourceEnforcement();
    if (!probe.cgroup_cpu && !probe.cgroup_memory) return; // no delegated cgroup on this host
    TempDir tmp;
    auto cas = std::make_shared<zeromesh::store::CasStore>(tmp / "cas");
    auto merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    zeromesh::exec::SandboxRunner runner(cas, merkle, tmp / "scratch");
    std::vector<std::pair<std::filesystem::path, bool>> events;
    runner.SetCgroupObserver([&](const std::filesystem::path& dir, bool added) { events.emplace_back(dir, added); });
    zeromesh::exec::RunSpec spec;
    spec.argv = {"true"};
    spec.cores = 1;
    auto res = runner.Run(spec, nullptr, {});
    if (!res.error.empty()) return; // sandbox unavailable here
    ASSERT_EQ(events.size(), static_cast<size_t>(2));
    ASSERT_TRUE(events[0].second);
    ASSERT_TRUE(!events[1].second);
    ASSERT_EQ(events[0].first, events[1].first);
}

TEST(GovernorFreezesARealCgroupAndThawsIt) {
    // Needs a delegated cgroup v2 subtree with the cpu controller (a systemd user session has one).
    auto self = zeromesh::exec::DelegatedCgroupParent();
    if (!self) return;
    const auto dir = *self / ("zm-comfort-test-" + std::to_string(::getpid()));
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);
    if (ec) return;

    pid_t child = ::fork();
    if (child == 0) {
        for (;;) ::pause();
    }
    {
        std::ofstream(dir / "cgroup.procs") << child;
    }
    auto frozen = [&] {
        std::ifstream in(dir / "cgroup.events");
        std::string k;
        int v;
        while (in >> k >> v) {
            if (k == "frozen") return v == 1;
        }
        return false;
    };
    auto wait_for = [&](bool want) {
        for (int i = 0; i < 100; ++i) {
            if (frozen() == want) return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    };

    CgroupActuator act;
    act.Add(dir);
    act.Apply(4);
    const bool did_freeze = wait_for(true);
    act.Apply(0);
    const bool did_thaw = wait_for(false);

    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    std::filesystem::remove(dir, ec);

    ASSERT_TRUE(act.Capabilities().freeze);
    ASSERT_TRUE(did_freeze);
    ASSERT_TRUE(did_thaw);
}
