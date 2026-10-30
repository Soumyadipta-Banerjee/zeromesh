// Freeze-first departure (v2 blueprint, claim 3): the departure reason carried in drain
// notices, runtime history, the keep/freeze plan, lid/suspend signals, the donor-side
// freeze -> ttl -> kill state machine, and a requester that waits for a frozen donor.

#include "tests/unit/node_fixture.hpp"

#include "exec/sandbox_runner.hpp"
#include "node/action_cache.hpp"
#include "node/departure.hpp"
#include "node/donor_policy.hpp"
#include "node/job_manager.hpp"
#include "sched/placer.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
namespace v1 = zeromesh::v1;
using zeromesh::node::DepartureWatcher;
using zeromesh::node::FormatDepartureReason;
using zeromesh::node::ParseDepartureReason;
using zeromesh::node::PlanDeparture;
using zeromesh::node::RunningTask;
using zeromesh::node::RuntimeHistory;
using zeromesh::node::SignalState;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;

namespace {

using Clock = std::chrono::steady_clock;

void Expect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

v1::TaskSpec Shell(const std::string& script) {
    v1::TaskSpec t;
    t.set_name("t");
    t.add_argv("sh");
    t.add_argv("-c");
    t.add_argv(script);
    return t;
}

// Records every freeze/thaw the service asks for.
struct FakeFreezer {
    std::mutex mu;
    std::vector<std::pair<std::string, bool>> calls;
    bool ok{true};

    zeromesh::node::NodeService::TaskFreezer Fn() {
        return [this](const std::string& id, bool freeze) {
            std::lock_guard lock(mu);
            calls.emplace_back(id, freeze);
            return ok;
        };
    }
    int Count(const std::string& id, bool freeze) {
        std::lock_guard lock(mu);
        int n = 0;
        for (const auto& c : calls) n += (c.first == id && c.second == freeze) ? 1 : 0;
        return n;
    }
    size_t Total() {
        std::lock_guard lock(mu);
        return calls.size();
    }
};

// One task running on donor `b` for requester `a`, driven through the real NodeApi.
struct RemoteRun {
    std::shared_ptr<zeromesh::node::NodeClient> client;
    std::string id;
    std::thread th;
    std::optional<v1::TaskResult> result;
    std::string run_err;
    std::atomic<bool> running{false};

    RemoteRun(TestNodeFull& a, TestNodeFull& b, std::string task_id, const v1::TaskSpec& spec)
        : client(a.ClientTo(b)), id(std::move(task_id)) {
        std::string err;
        auto reply = client->Offer(id, spec, {}, &err);
        Expect(reply.has_value() && reply->accepted(), err + (reply ? reply->reason() : ""));
        th = std::thread([this] {
            result = client->Run(
                id,
                [this](const v1::JobEvent& ev) {
                    if (ev.state() == v1::RUNNING) running = true;
                },
                &run_err);
        });
        const auto deadline = Clock::now() + 10s;
        while (!running && Clock::now() < deadline) std::this_thread::sleep_for(10ms);
        Expect(running.load(), "task never started running");
        std::this_thread::sleep_for(150ms); // let the sandbox come up
    }

    void Join() {
        if (th.joinable()) th.join();
    }
    ~RemoteRun() {
        if (th.joinable()) {
            client->Cancel(id);
            th.join();
        }
    }
};

struct Requester {
    std::shared_ptr<zeromesh::sched::Placer> placer;
    std::unique_ptr<zeromesh::node::JobManager> jm;

    explicit Requester(TestNodeFull& self) {
        auto factory = [&self](const zeromesh::net::Peer& p) {
            return std::make_shared<zeromesh::node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port),
                                                                self.id, self.pins, p.id);
        };
        placer = std::make_shared<zeromesh::sched::Placer>(self.executor, self.peers, factory, self.cas, self.merkle,
                                                           self.node_id, [&self] { return self.telemetry; });
        jm = std::make_unique<zeromesh::node::JobManager>(
            self.cas, self.merkle, std::make_shared<zeromesh::node::ActionCache>(self.cas), placer);
    }
};

bool CgroupFrozen(const std::filesystem::path& dir) {
    std::ifstream in(dir / "cgroup.events");
    std::string k;
    int v = 0;
    while (in >> k >> v) {
        if (k == "frozen") return v == 1;
    }
    return false;
}

bool WaitFrozen(const std::filesystem::path& dir, bool want) {
    for (int i = 0; i < 100; ++i) {
        if (CgroupFrozen(dir) == want) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

} // namespace

// ---- departure reason carried in drain notices ----------------------------------------------

TEST(DepartureReasonRoundTripsThroughTheDrainString) {
    const std::string s = FormatDepartureReason(v1::DEPARTURE_LID, 120000);
    auto p = ParseDepartureReason(s);
    ASSERT_TRUE(p.has_value());
    ASSERT_EQ(static_cast<int>(p->code), static_cast<int>(v1::DEPARTURE_LID));
    ASSERT_EQ(p->ttl_ms, static_cast<uint32_t>(120000));
}

TEST(DepartureReasonKeepsFreeTextAndIgnoresPlainReasons) {
    const std::string s = FormatDepartureReason(v1::DEPARTURE_BATTERY, 5000, "battery at 12%");
    ASSERT_TRUE(s.find("battery at 12%") != std::string::npos);
    auto p = ParseDepartureReason(s);
    ASSERT_TRUE(p.has_value());
    ASSERT_EQ(p->ttl_ms, static_cast<uint32_t>(5000));
    // Without a ttl nothing is frozen, so the string is a plain drain reason.
    ASSERT_FALSE(ParseDepartureReason(FormatDepartureReason(v1::DEPARTURE_BATTERY, 0)).has_value());
    ASSERT_FALSE(ParseDepartureReason("battery at 12% (drain below 20%)").has_value());
    ASSERT_FALSE(ParseDepartureReason("").has_value());
}

// ---- runtime history ------------------------------------------------------------------------

TEST(RuntimeHistorySmoothsDurationsAndSurvivesRestart) {
    TempDir tmp;
    const auto file = tmp / "runtime_history.tsv";
    v1::TaskSpec spec = Shell("render frame");
    v1::TaskSpec other = Shell("something else");
    {
        RuntimeHistory h(file);
        ASSERT_FALSE(h.Expected(spec).has_value());
        h.Record(spec, 10000);
        ASSERT_EQ(h.Expected(spec).value(), static_cast<int64_t>(10000));
        h.Record(spec, 20000);
        ASSERT_EQ(h.Expected(spec).value(), static_cast<int64_t>(15000)); // average of old and new
        ASSERT_FALSE(h.Expected(other).has_value());
    }
    RuntimeHistory reloaded(file);
    ASSERT_EQ(reloaded.Expected(spec).value(), static_cast<int64_t>(15000));
}

TEST(RuntimeHistoryTreatsAnUnsetEnvNameAsHost) {
    RuntimeHistory h;
    v1::TaskSpec raw = Shell("x");
    v1::TaskSpec normalized = raw;
    normalized.mutable_env_spec()->set_name("host");
    h.Record(raw, 1234);
    ASSERT_EQ(h.Expected(normalized).value(), static_cast<int64_t>(1234));
}

// ---- the plan: what keeps running, what freezes ---------------------------------------------

TEST(PlanKeepsTasksThatFitTheGraceWindowAndFreezesTheRest) {
    std::vector<RunningTask> tasks = {
        {"fits", 8000, 10000, true},      // 2 s left, grace 5 s
        {"long", 5000, 60000, true},      // 55 s left
        {"unknown", 1000, std::nullopt, true},
        {"overran", 9000, 3000, true},    // already past its estimate: finishing is imminent
        {"pinned", 0, std::nullopt, false} // not preemptible: never touched
    };
    auto plan = PlanDeparture(tasks, 5000ms);
    auto has = [](const std::vector<std::string>& v, const std::string& s) {
        for (const auto& x : v)
            if (x == s) return true;
        return false;
    };
    ASSERT_TRUE(has(plan.keep, "fits"));
    ASSERT_TRUE(has(plan.keep, "overran"));
    ASSERT_TRUE(has(plan.keep, "pinned"));
    ASSERT_TRUE(has(plan.freeze, "long"));
    ASSERT_TRUE(has(plan.freeze, "unknown"));
    ASSERT_EQ(plan.keep.size(), static_cast<size_t>(3));
    ASSERT_EQ(plan.freeze.size(), static_cast<size_t>(2));
}

// ---- departure signals ----------------------------------------------------------------------

TEST(WatcherReportsLidCloseAndOpenOnce) {
    DepartureWatcher w;
    ASSERT_FALSE(w.Update(SignalState{}).has_value());
    SignalState closed;
    closed.lid_closed = true;
    auto ev = w.Update(closed);
    ASSERT_TRUE(ev.has_value() && ev->active);
    ASSERT_EQ(static_cast<int>(ev->reason), static_cast<int>(v1::DEPARTURE_LID));
    ASSERT_FALSE(w.Update(closed).has_value()); // no repeat while it stays closed
    auto back = w.Update(SignalState{});
    ASSERT_TRUE(back.has_value() && !back->active);
    ASSERT_EQ(static_cast<int>(back->reason), static_cast<int>(v1::DEPARTURE_LID));
}

TEST(WatcherSuspendOutranksLidAndUnplugIsOptIn) {
    DepartureWatcher w;
    SignalState both;
    both.lid_closed = true;
    both.sleep_pending = true;
    auto ev = w.Update(both);
    ASSERT_TRUE(ev.has_value());
    ASSERT_EQ(static_cast<int>(ev->reason), static_cast<int>(v1::DEPARTURE_SUSPEND));

    SignalState unplugged;
    unplugged.on_ac = false;
    DepartureWatcher off(/*depart_on_lid=*/true, /*depart_on_unplug=*/false);
    ASSERT_FALSE(off.Update(unplugged).has_value());
    DepartureWatcher on(/*depart_on_lid=*/true, /*depart_on_unplug=*/true);
    auto up = on.Update(unplugged);
    ASSERT_TRUE(up.has_value() && up->active);
    ASSERT_EQ(static_cast<int>(up->reason), static_cast<int>(v1::DEPARTURE_UNPLUG));
}

TEST(WatcherIgnoresLidWhenDisabled) {
    DepartureWatcher w(/*depart_on_lid=*/false, /*depart_on_unplug=*/false);
    SignalState closed;
    closed.lid_closed = true;
    ASSERT_FALSE(w.Update(closed).has_value());
}

TEST(LidStateIsReadFromProcAcpi) {
    TempDir root;
    ASSERT_FALSE(zeromesh::node::ReadLidClosed(root.path()).has_value()); // no lid on this machine
    zeromesh::testing::WriteFile(root / "proc/acpi/button/lid/LID0/state", "state:      closed\n");
    ASSERT_EQ(zeromesh::node::ReadLidClosed(root.path()).value(), true);
    zeromesh::testing::WriteFile(root / "proc/acpi/button/lid/LID0/state", "state:      open\n");
    ASSERT_EQ(zeromesh::node::ReadLidClosed(root.path()).value(), false);
}

TEST(PrepareForSleepIsParsedFromBusctlJson) {
    const std::string head =
        R"({"type":"signal","endian":"l","flags":1,"version":1,"cookie":7,"sender":":1.2",)"
        R"("path":"/org/freedesktop/login1","interface":"org.freedesktop.login1.Manager",)"
        R"("member":"PrepareForSleep","payload":{"type":"b","data":[)";
    ASSERT_EQ(zeromesh::node::ParsePrepareForSleep(head + "true]}}").value(), true);
    ASSERT_EQ(zeromesh::node::ParsePrepareForSleep(head + "false]}}").value(), false);
    ASSERT_FALSE(zeromesh::node::ParsePrepareForSleep(R"({"member":"SessionNew","payload":{"data":[true]}})")
                     .has_value());
    ASSERT_FALSE(zeromesh::node::ParsePrepareForSleep("not json").has_value());
}

TEST(LogindMonitorDeliversSleepSignalsFromItsCommand) {
    // A stand-in for `busctl monitor`: two signals, then it idles like the real one does.
    const std::string line_true =
        R"({"member":"PrepareForSleep","payload":{"type":"b","data":[true]}})";
    const std::string line_false =
        R"({"member":"PrepareForSleep","payload":{"type":"b","data":[false]}})";
    zeromesh::node::LogindMonitor::Config cfg;
    cfg.monitor_cmd = "printf '%s\\n%s\\n' '" + line_true + "' '" + line_false + "'; sleep 30";
    cfg.inhibit_cmd = ""; // no delay inhibitor in the test
    std::mutex mu;
    std::vector<bool> seen;
    zeromesh::node::LogindMonitor mon(cfg, [&](bool preparing) {
        std::lock_guard lock(mu);
        seen.push_back(preparing);
    });
    mon.Start();
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
        {
            std::lock_guard lock(mu);
            if (seen.size() >= 2) break;
        }
        std::this_thread::sleep_for(20ms);
    }
    const auto t0 = Clock::now();
    mon.Stop(); // must kill the idling child promptly
    Expect(Clock::now() - t0 < 2s, "Stop() did not return promptly");
    std::lock_guard lock(mu);
    ASSERT_EQ(seen.size(), static_cast<size_t>(2));
    ASSERT_EQ(seen[0], true);
    ASSERT_EQ(seen[1], false);
}

// ---- donor policy keys ----------------------------------------------------------------------

TEST(DonorPolicyParsesDepartureKeys) {
    std::ostringstream warn;
    auto p = zeromesh::node::DonorPolicy::Parse(
        "drain_grace_s = 30\nfreeze_ttl_s = 45\ndepart_on_lid = false\ndepart_on_unplug = true\n", warn);
    ASSERT_EQ(p.drain_grace_s, 30);
    ASSERT_EQ(p.freeze_ttl_s, 45);
    ASSERT_EQ(p.depart_on_lid, false);
    ASSERT_EQ(p.depart_on_unplug, true);
    ASSERT_EQ(warn.str(), std::string(""));
    zeromesh::node::DonorPolicy defaults;
    ASSERT_EQ(defaults.drain_grace_s, 20);
    ASSERT_EQ(defaults.freeze_ttl_s, 120);
    ASSERT_EQ(defaults.depart_on_lid, true);
    ASSERT_EQ(defaults.depart_on_unplug, false);
}

// ---- NodeService: freeze, ttl, thaw ---------------------------------------------------------

TEST(DepartureFreezesUnknownTasksAndKillsThemWhenTheTtlExpires) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    FakeFreezer fz;
    b.service->SetTaskFreezer(fz.Fn());
    RemoteRun run(a, b, "t1", Shell("sleep 30"));

    const auto t0 = Clock::now();
    auto rep = b.service->BeginDeparture(v1::DEPARTURE_LID, 500ms, 0ms, t0);
    ASSERT_EQ(rep.frozen, 1);
    ASSERT_EQ(rep.kept, 0);
    ASSERT_EQ(rep.cancelled, 0);
    ASSERT_EQ(fz.Count("node-a/t1", true), 1);
    ASSERT_TRUE(b.service->IsDraining());
    ASSERT_TRUE(b.service->Departing());

    ASSERT_EQ(b.service->TickDeparture(t0 + 100ms), 0); // inside the ttl: stays frozen
    ASSERT_EQ(fz.Count("node-a/t1", false), 0);
    ASSERT_EQ(b.service->TickDeparture(t0 + 600ms), 1); // ttl over: kill, the requester re-dispatches
    run.Join();
    ASSERT_TRUE(run.result.has_value());
    ASSERT_TRUE(run.result->cancelled());
}

TEST(DepartureEndThawsFrozenTasksAndLetsThemFinish) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    FakeFreezer fz;
    b.service->SetTaskFreezer(fz.Fn());
    RemoteRun run(a, b, "t1", Shell("sleep 1; echo survived"));

    const auto t0 = Clock::now();
    auto rep = b.service->BeginDeparture(v1::DEPARTURE_LID, 60000ms, 0ms, t0);
    ASSERT_EQ(rep.frozen, 1);
    ASSERT_EQ(b.service->TickDeparture(t0 + 5s), 0);
    ASSERT_EQ(b.service->EndDeparture(), 1);
    ASSERT_EQ(fz.Count("node-a/t1", false), 1);
    ASSERT_FALSE(b.service->IsDraining());
    ASSERT_FALSE(b.service->Departing());
    run.Join(); // the (fake) freeze never stopped the process; it runs to completion
    ASSERT_TRUE(run.result.has_value());
    ASSERT_EQ(run.result->exit_code(), 0);
    ASSERT_EQ(run.result->stdout_head(), std::string("survived\n"));
}

TEST(DepartureKeepsATaskThatFitsTheGraceWindow) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    FakeFreezer fz;
    b.service->SetTaskFreezer(fz.Fn());
    auto history = std::make_shared<RuntimeHistory>();
    const v1::TaskSpec spec = Shell("sleep 30");
    history->Record(spec, 2000); // this command usually takes 2 s
    b.service->SetRuntimeHistory(history);
    RemoteRun run(a, b, "t1", spec);

    const auto t0 = Clock::now();
    auto rep = b.service->BeginDeparture(v1::DEPARTURE_UNPLUG, 60000ms, 10000ms, t0);
    ASSERT_EQ(rep.kept, 1);
    ASSERT_EQ(rep.frozen, 0);
    ASSERT_EQ(fz.Total(), static_cast<size_t>(0));
    ASSERT_EQ(b.service->TickDeparture(t0 + 5s), 0); // still inside the grace window
    ASSERT_EQ(fz.Total(), static_cast<size_t>(0));
}

TEST(DepartureFreezesAKeptTaskOnceTheGraceWindowRunsOut) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    FakeFreezer fz;
    b.service->SetTaskFreezer(fz.Fn());
    auto history = std::make_shared<RuntimeHistory>();
    const v1::TaskSpec spec = Shell("sleep 30");
    history->Record(spec, 100); // the estimate was wrong: it is still running after the grace
    b.service->SetRuntimeHistory(history);
    RemoteRun run(a, b, "t1", spec);

    const auto t0 = Clock::now();
    auto rep = b.service->BeginDeparture(v1::DEPARTURE_UNPLUG, 1000ms, 500ms, t0);
    ASSERT_EQ(rep.kept, 1);
    ASSERT_EQ(b.service->TickDeparture(t0 + 700ms), 0);
    ASSERT_EQ(fz.Count("node-a/t1", true), 1); // grace over: frozen now, ttl counts from here
    ASSERT_EQ(b.service->TickDeparture(t0 + 1000ms), 0);
    ASSERT_EQ(b.service->TickDeparture(t0 + 1800ms), 1); // 700 ms + 1000 ms ttl
    run.Join();
    ASSERT_TRUE(run.result.has_value() && run.result->cancelled());
}

TEST(DepartureNeverTouchesANonPreemptibleTask) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    FakeFreezer fz;
    b.service->SetTaskFreezer(fz.Fn());
    v1::TaskSpec spec = Shell("sleep 30");
    spec.mutable_policy()->set_preemptible(false);
    RemoteRun run(a, b, "t1", spec);

    const auto t0 = Clock::now();
    auto rep = b.service->BeginDeparture(v1::DEPARTURE_BATTERY, 100ms, 0ms, t0);
    ASSERT_EQ(rep.kept, 1);
    ASSERT_EQ(rep.frozen, 0);
    ASSERT_EQ(b.service->TickDeparture(t0 + 5s), 0);
    ASSERT_EQ(fz.Total(), static_cast<size_t>(0));
}

TEST(DepartureFallsBackToKillingWhenTasksCannotBeFrozen) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    FakeFreezer fz;
    fz.ok = false; // no cgroup freezer on this host
    b.service->SetTaskFreezer(fz.Fn());
    RemoteRun run(a, b, "t1", Shell("sleep 30"));

    auto rep = b.service->BeginDeparture(v1::DEPARTURE_LID, 60000ms, 0ms);
    ASSERT_EQ(rep.frozen, 0);
    ASSERT_EQ(rep.cancelled, 1);
    run.Join();
    ASSERT_TRUE(run.result.has_value() && run.result->cancelled());
}

TEST(DepartureWithoutAFreezerBehavesLikeTheOldDrain) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    RemoteRun run(a, b, "t1", Shell("sleep 30"));
    auto rep = b.service->BeginDeparture(v1::DEPARTURE_BATTERY, 60000ms, 0ms);
    ASSERT_EQ(rep.cancelled, 1);
    run.Join();
    ASSERT_TRUE(run.result.has_value() && run.result->cancelled());
}

TEST(DepartureRefusesNewOffers) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    b.service->BeginDeparture(v1::DEPARTURE_LID, 1000ms, 0ms);
    std::string err;
    auto reply = a.ClientTo(b)->Offer("late", Shell("true"), {}, &err);
    ASSERT_TRUE(reply.has_value());
    ASSERT_FALSE(reply->accepted());
    ASSERT_TRUE(reply->reason().find("draining") != std::string::npos);
    b.service->EndDeparture();
    auto again = a.ClientTo(b)->Offer("later", Shell("true"), {}, &err);
    ASSERT_TRUE(again.has_value() && again->accepted());
}

TEST(DrainNoticeCarriesReasonCodeAndTtlToThePeerTable) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    b.peers->Upsert(a.AsPeer()); // b knows a, so a's notice is recorded
    std::string err;
    Expect(a.ClientTo(b)->Drain("lid closed", &err, v1::DEPARTURE_LID, 5000, 0), err);
    auto peer = b.peers->Get("node-a");
    ASSERT_TRUE(peer.has_value());
    auto parsed = ParseDepartureReason(peer->draining_reason);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(static_cast<int>(parsed->code), static_cast<int>(v1::DEPARTURE_LID));
    ASSERT_EQ(parsed->ttl_ms, static_cast<uint32_t>(5000));

    // A notice without a code stays a plain, kill-first drain.
    Expect(a.ClientTo(b)->Drain("battery", &err), err);
    auto plain = b.peers->Get("node-a");
    ASSERT_TRUE(plain.has_value());
    ASSERT_FALSE(ParseDepartureReason(plain->draining_reason).has_value());
}

// ---- real kernel freezer through the sandbox runner -----------------------------------------

TEST(DepartureFreezesARealSandboxCgroupAndItResumes) {
    if (!zeromesh::exec::DelegatedCgroupParent()) return; // no delegated cgroup v2 subtree here
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    b.service->SetTaskFreezer(
        [&b](const std::string& id, bool freeze) { return b.runner->SetTaskFrozen(id, freeze); });
    RemoteRun run(a, b, "t1", Shell("sleep 2; echo done"));

    std::optional<std::filesystem::path> dir;
    for (int i = 0; i < 100 && !dir; ++i) {
        dir = b.runner->CgroupFor("node-a/t1");
        if (!dir) std::this_thread::sleep_for(20ms);
    }
    if (!dir) return; // the sandbox did not get a task cgroup (e.g. bwrap missing): nothing to test

    auto rep = b.service->BeginDeparture(v1::DEPARTURE_LID, 60000ms, 0ms);
    ASSERT_EQ(rep.frozen, 1);
    Expect(WaitFrozen(*dir, true), "cgroup never reported frozen");
    ASSERT_EQ(b.service->EndDeparture(), 1);
    Expect(WaitFrozen(*dir, false), "cgroup never thawed");
    run.Join();
    ASSERT_TRUE(run.result.has_value());
    ASSERT_EQ(run.result->exit_code(), 0);
    ASSERT_EQ(run.result->stdout_head(), std::string("done\n"));
}

// ---- requester side: a freeze-first donor is waited for, not cancelled ----------------------

TEST(PlacerWaitsForAFreezeFirstDonorInsteadOfCancelling) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    v1::JobSpec job;
    job.set_name("j");
    auto* t = job.add_tasks();
    *t = Shell("sleep 2; echo finished");
    t->set_name("t");
    t->mutable_policy()->set_may_run_local(false);
    const std::string id = req.jm->Submit(job);
    auto stream = req.jm->Watch(id);
    bool running = false;
    const auto start_deadline = Clock::now() + 10s;
    while (!running && Clock::now() < start_deadline) {
        auto ev = stream->Next(100ms);
        if (ev && ev->state() == v1::RUNNING) running = true;
    }
    ASSERT_TRUE(running);

    // The donor announces a freeze-first departure; the placer must not cancel the run.
    a.peers->MarkDraining("node-b", FormatDepartureReason(v1::DEPARTURE_LID, 60000));

    const auto done_deadline = Clock::now() + 30s;
    while (!req.jm->Get(id)->done() && Clock::now() < done_deadline) std::this_thread::sleep_for(50ms);
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    Expect(st->tasks().at("t") == v1::SUCCEEDED, st->results().at("t").error());
    ASSERT_EQ(st->results().at("t").executed_by(), std::string("node-b"));
    ASSERT_EQ(st->results().at("t").stdout_head(), std::string("finished\n"));
    while (auto ev = stream->Next(10ms)) {
        ASSERT_FALSE(ev->state() == v1::QUEUED && ev->detail().find("retry ") == 0);
    }
}
