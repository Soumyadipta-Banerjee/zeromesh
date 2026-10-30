#include "tests/unit/test_framework.hpp"
#include "tests/unit/node_fixture.hpp"
#include "tests/unit/test_util.hpp"

#include "net/peer_table.hpp"
#include "sched/hazard.hpp"

#include <chrono>
#include <cmath>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using zeromesh::net::Peer;
using zeromesh::net::PeerTable;
using zeromesh::net::PresenceEvent;
using zeromesh::sched::HazardModel;

namespace {

constexpr double kHour = 3600.0;

// Appends one finished session for `id`: joins at `t`, ends `duration_s` later either as an
// abrupt departure (`departed`) or a graceful drain (censored). Advances `t` past a short gap.
void Session(HazardModel& m, const std::string& id, double& t, double duration_s, bool departed) {
    m.NoteJoin(id, t);
    if (departed) {
        m.NoteLeave(id, t + duration_s);
    } else {
        m.NoteDrain(id, t + duration_s);
    }
    t += duration_s + 10.0;
}

// A node that departs as a Poisson process with `rate_per_h`: sessions last Exp(rate).
void PoissonChurn(HazardModel& m, const std::string& id, double rate_per_h, int sessions, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::exponential_distribution<double> life_h(rate_per_h);
    double t = 1.0e6;
    for (int i = 0; i < sessions; ++i) Session(m, id, t, std::max(life_h(rng), 1.0 / kHour) * kHour, true);
}

bool Near(double a, double b, double abs_tol) { return std::fabs(a - b) <= abs_tol; }

} // namespace

TEST(Hazard_NoHistoryFallsBackToDefaultExponential) {
    HazardModel::Config cfg;
    cfg.default_hazard_per_hour = 0.25;
    HazardModel m({}, cfg);
    ASSERT_FALSE(m.HasHistory());
    ASSERT_TRUE(Near(m.HazardPerHour("nobody"), 0.25, 1e-9));
    // S(1h) = exp(-0.25)
    ASSERT_TRUE(Near(m.SurvivalProb("nobody", kHour), std::exp(-0.25), 1e-9));
    ASSERT_EQ(m.SurvivalProb("nobody", 0.0), 1.0);
    // E[min(L, T)] for exponential L = (1 - exp(-lambda T)) / lambda, in hours here.
    const double expect_s = (1.0 - std::exp(-0.25)) / 0.25 * kHour;
    const double got = m.ExpectedMin("nobody", kHour);
    ASSERT_TRUE(std::fabs(got - expect_s) / expect_s < 1e-3);
    ASSERT_TRUE(got <= kHour);
    ASSERT_EQ(m.ExpectedMin("nobody", 0.0), 0.0);
}

TEST(Hazard_PoissonChurnConvergesToTrueRate) {
    for (double rate : {0.5, 2.0}) {
        HazardModel m;
        PoissonChurn(m, "n1", rate, 2000, 42);
        ASSERT_TRUE(m.HasHistory());
        // Only the newest max_obs_per_node (1000) sessions are kept; every one of them is a departure.
        ASSERT_EQ(m.Events("n1"), static_cast<size_t>(1000));
        const double est = m.HazardPerHour("n1");
        ASSERT_TRUE(std::fabs(est - rate) / rate < 0.15);
        // Survival over half an hour: exp(-rate/2).
        ASSERT_TRUE(Near(m.SurvivalProb("n1", 0.5 * kHour), std::exp(-rate * 0.5), 0.05));
    }
}

TEST(Hazard_PooledEstimateAcrossNodesAndUnknownNodeUsesIt) {
    HazardModel m;
    PoissonChurn(m, "a", 1.0, 700, 1);
    PoissonChurn(m, "b", 1.0, 700, 2);
    PoissonChurn(m, "c", 1.0, 700, 3);
    // A node never seen gets the pooled estimate (no node data to weigh in).
    const double pooled = m.HazardPerHour("never-seen");
    ASSERT_TRUE(std::fabs(pooled - 1.0) < 0.15);
    ASSERT_TRUE(Near(m.SurvivalProb("never-seen", kHour), std::exp(-pooled), 0.05));
}

TEST(Hazard_CensoringLowersTheEstimate) {
    // Same 40 session lengths, 10 minutes apart. Model A: every session ends in a departure.
    // Model B: sessions 1, 3 and 5 ended in a graceful drain (censored), the rest are departures.
    HazardModel a, b;
    double ta = 0, tb = 0;
    for (int i = 1; i <= 40; ++i) {
        const double d = 600.0 * i;
        Session(a, "n", ta, d, true);
        Session(b, "n", tb, d, !(i == 1 || i == 3 || i == 5));
    }
    ASSERT_EQ(a.Events("n"), static_cast<size_t>(40));
    ASSERT_EQ(b.Events("n"), static_cast<size_t>(37));
    ASSERT_EQ(b.Observations("n"), static_cast<size_t>(40));
    ASSERT_TRUE(b.HazardPerHour("n") < a.HazardPerHour("n"));
    ASSERT_TRUE(b.SurvivalProb("n", kHour) > a.SurvivalProb("n", kHour));
}

TEST(Hazard_DrainOnlyNodeIsNotCountedAsDeparting) {
    HazardModel m;
    double tf = 0, ts = 0;
    for (int i = 0; i < 50; ++i) {
        Session(m, "flaky", tf, 1800.0, true);
        Session(m, "polite", ts, 10 * kHour, false); // drains gracefully, never vanishes
    }
    ASSERT_EQ(m.Events("polite"), static_cast<size_t>(0));
    ASSERT_EQ(m.Events("flaky"), static_cast<size_t>(50));
    ASSERT_TRUE(m.HazardPerHour("polite") < m.HazardPerHour("flaky"));
    ASSERT_TRUE(m.ExpectedMin("polite", kHour) > m.ExpectedMin("flaky", kHour));
}

TEST(Hazard_FewSamplesAreShrunkTowardThePool) {
    HazardModel m;
    double tb = 0;
    for (int i = 0; i < 200; ++i) Session(m, "stable", tb, 5 * kHour, true);
    // One lone short session: taken alone it would say "2 departures per hour".
    double tx = 0;
    Session(m, "x", tx, 0.5 * kHour, true);

    const double pooled = m.HazardPerHour("never-seen");
    const double shrunk = m.HazardPerHour("x");
    ASSERT_TRUE(shrunk > pooled);  // its own evidence counts for something
    ASSERT_TRUE(shrunk < 1.0);     // but far from the raw single-sample estimate of 2/h
}

TEST(Hazard_FlakyNodeHasShorterExpectedWorkThanStable) {
    HazardModel m;
    double tf = 0, ts = 0;
    for (int i = 0; i < 60; ++i) {
        Session(m, "flaky", tf, 0.5 * kHour, true);
        Session(m, "stable", ts, 20 * kHour, true);
    }
    const double task = kHour;
    const double ef = m.ExpectedMin("flaky", task);
    const double es = m.ExpectedMin("stable", task);
    ASSERT_TRUE(ef < es);
    ASSERT_TRUE(es <= task);
    ASSERT_TRUE(ef > 0.0);
    ASSERT_TRUE(m.SurvivalProb("flaky", task) < m.SurvivalProb("stable", task));
}

TEST(Hazard_SurvivalAdapterMatchesDirectCall) {
    auto m = std::make_shared<HazardModel>();
    PoissonChurn(*m, "n", 1.0, 300, 7);
    auto fn = HazardModel::MakeSurvivalFn(m);
    ASSERT_TRUE(static_cast<bool>(fn));
    ASSERT_EQ(fn("n", 1800.0), m->SurvivalProb("n", 1800.0));
    ASSERT_EQ(fn("other", 60.0), m->SurvivalProb("other", 60.0));
}

TEST(Hazard_LeaveWithoutJoinIsIgnoredAndRejoinSemantics) {
    HazardModel m;
    m.NoteLeave("ghost", 100.0);
    m.NoteDrain("ghost", 100.0);
    ASSERT_EQ(m.Observations("ghost"), static_cast<size_t>(0));
    ASSERT_FALSE(m.HasHistory());

    // Join twice with no sign of life in between: the first session's fate is unknown, so it is
    // dropped rather than recorded as a bogus zero-length departure.
    m.NoteJoin("p", 1000.0);
    m.NoteJoin("p", 5000.0);
    ASSERT_EQ(m.Observations("p"), static_cast<size_t>(0));

    // With a recorded sign of life, the earlier session counts as a departure at that time.
    m.NoteJoin("q", 1000.0);
    m.NoteAlive("q", 1500.0);
    m.NoteJoin("q", 5000.0);
    ASSERT_EQ(m.Observations("q"), static_cast<size_t>(1));
    ASSERT_EQ(m.Events("q"), static_cast<size_t>(1));
}

TEST(Hazard_OpenSessionAgeConditionsSurvival) {
    // Two nodes whose closed sessions all lasted 1h with a departure; "old" has been up for
    // a week since, "new" just joined. Both are exponential-ish, so remaining life is similar,
    // but the open session must not break or inflate the estimate.
    double now = 0;
    HazardModel m({}, {}, [&now] { return now; });
    double t = 0;
    for (int i = 0; i < 30; ++i) {
        Session(m, "old", t, kHour, true);
        Session(m, "new", t, kHour, true);
    }
    m.NoteJoin("old", t);
    m.NoteJoin("new", t + 7 * 24 * kHour);
    now = t + 7 * 24 * kHour;
    const double s_old = m.SurvivalProb("old", kHour);
    const double s_new = m.SurvivalProb("new", kHour);
    ASSERT_TRUE(s_old > 0.0 && s_old <= 1.0);
    ASSERT_TRUE(s_new > 0.0 && s_new <= 1.0);
    // A week of uninterrupted uptime is evidence of stability: "old" is not less reliable.
    ASSERT_TRUE(s_old >= s_new - 1e-9);
}

TEST(Hazard_HistorySurvivesRestart) {
    zeromesh::testing::TempDir dir;
    const auto log = dir / "state/hazard.log";
    double want_h = 0;
    {
        HazardModel m(log);
        double t = 0;
        for (int i = 1; i <= 30; ++i) Session(m, "n1", t, 600.0 * i, i % 5 != 0);
        Session(m, "n2", t, 3 * kHour, false);
        ASSERT_EQ(m.Events("n1"), static_cast<size_t>(24));
        want_h = m.HazardPerHour("n1");
    }
    ASSERT_TRUE(std::filesystem::exists(log));
    HazardModel again(log);
    ASSERT_TRUE(again.HasHistory());
    ASSERT_EQ(again.Observations("n1"), static_cast<size_t>(30));
    ASSERT_EQ(again.Events("n1"), static_cast<size_t>(24));
    ASSERT_EQ(again.Observations("n2"), static_cast<size_t>(1));
    ASSERT_EQ(again.Events("n2"), static_cast<size_t>(0));
    ASSERT_TRUE(Near(again.HazardPerHour("n1"), want_h, 1e-9));

    // New events after a reload keep appending to the same file.
    double t = 1.0e7;
    Session(again, "n3", t, kHour, true);
    HazardModel third(log);
    ASSERT_EQ(third.Events("n3"), static_cast<size_t>(1));
}

TEST(Hazard_MalformedLogLinesAreSkipped) {
    zeromesh::testing::TempDir dir;
    const auto log = dir / "hazard.log";
    zeromesh::testing::WriteFile(log, "garbage\nJ n 100\nL n notanumber\nL n 700\nX n 5\nJ\nL n 900\n");
    HazardModel m(log);
    ASSERT_EQ(m.Observations("n"), static_cast<size_t>(1));
    ASSERT_EQ(m.Events("n"), static_cast<size_t>(1));
}

// ---- PeerTable presence events -> hazard log ----

TEST(PeerTable_PresenceEventsJoinLeaveDrain) {
    PeerTable table;
    std::vector<std::pair<std::string, PresenceEvent>> seen;
    table.RegisterPresenceCallback(
        [&](const std::string& id, PresenceEvent ev) { seen.emplace_back(id, ev); });

    Peer p;
    p.id = "n1";
    table.Upsert(p);
    table.Upsert(p); // heartbeat, not a new join
    ASSERT_EQ(seen.size(), static_cast<size_t>(1));
    ASSERT_TRUE(seen[0].second == PresenceEvent::Joined);

    table.Remove("n1"); // an active peer vanishing is a departure
    ASSERT_EQ(seen.size(), static_cast<size_t>(2));
    ASSERT_TRUE(seen[1].second == PresenceEvent::Left);
    table.Remove("n1"); // already gone
    ASSERT_EQ(seen.size(), static_cast<size_t>(2));

    table.Upsert(p);
    table.MarkDraining("n1", "unplug"); // graceful: censored
    ASSERT_EQ(seen.size(), static_cast<size_t>(4));
    ASSERT_TRUE(seen[3].second == PresenceEvent::Drained);
    table.MarkDraining("n1", "unplug again");
    ASSERT_EQ(seen.size(), static_cast<size_t>(4));
    table.Remove("n1"); // the drain already ended the session; no second event
    ASSERT_EQ(seen.size(), static_cast<size_t>(4));

    // A peer announcing itself as draining straight away: join, then drained.
    Peer d;
    d.id = "n2";
    d.state = zeromesh::net::PeerState::Draining;
    table.Upsert(d);
    ASSERT_EQ(seen.size(), static_cast<size_t>(6));
    ASSERT_TRUE(seen[4].second == PresenceEvent::Joined);
    ASSERT_TRUE(seen[5].second == PresenceEvent::Drained);

    // Drained peer comes back Active: a new session.
    Peer back = d;
    back.state = zeromesh::net::PeerState::Active;
    table.Upsert(back);
    ASSERT_EQ(seen.size(), static_cast<size_t>(7));
    ASSERT_TRUE(seen[6].second == PresenceEvent::Joined);
}

TEST(PeerTable_PruneStaleIsADepartureAndClearIsCensored) {
    PeerTable table;
    std::vector<PresenceEvent> seen;
    table.RegisterPresenceCallback([&](const std::string&, PresenceEvent ev) { seen.push_back(ev); });
    Peer a, b;
    a.id = "a";
    b.id = "b";
    table.Upsert(a);
    table.Upsert(b);
    ASSERT_EQ(seen.size(), static_cast<size_t>(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_EQ(table.PruneStale(std::chrono::milliseconds(1)), static_cast<size_t>(2));
    ASSERT_EQ(seen.size(), static_cast<size_t>(4));
    ASSERT_TRUE(seen[2] == PresenceEvent::Left);
    ASSERT_TRUE(seen[3] == PresenceEvent::Left);

    table.Upsert(a);
    table.Clear(); // we left the team: not the peer's doing
    ASSERT_EQ(seen.size(), static_cast<size_t>(6));
    ASSERT_TRUE(seen[5] == PresenceEvent::Drained);
}

TEST(PeerTable_FeedsHazardModelViaObservePeers) {
    double now = 1000.0;
    auto model = std::make_shared<HazardModel>(std::filesystem::path{}, HazardModel::Config{},
                                               [&now] { return now; });
    PeerTable table;
    zeromesh::sched::ObservePeers(table, model);

    Peer p;
    p.id = "laptop";
    table.Upsert(p);
    now = 1600.0;
    table.Remove("laptop");
    ASSERT_EQ(model->Observations("laptop"), static_cast<size_t>(1));
    ASSERT_EQ(model->Events("laptop"), static_cast<size_t>(1));

    now = 2000.0;
    table.Upsert(p);
    now = 2500.0;
    table.MarkDraining("laptop", "lid");
    ASSERT_EQ(model->Observations("laptop"), static_cast<size_t>(2));
    ASSERT_EQ(model->Events("laptop"), static_cast<size_t>(1)); // the drain is censored
}

// ---- OfferReply.hazard_per_hour ----

TEST(OfferReplyCarriesDonorHazardWhenKnown) {
    auto key = zeromesh::net::GenerateTeamKey();
    zeromesh::testing::TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);

    zeromesh::v1::TaskSpec spec;
    spec.set_name("h");
    spec.add_argv("sh");
    spec.add_argv("-c");
    spec.add_argv("echo h");
    spec.mutable_policy()->set_may_run_local(false);

    auto client = a.ClientTo(b);
    std::string err;
    auto none = client->Offer("t1", spec, {}, &err);
    ASSERT_TRUE(none.has_value());
    ASSERT_TRUE(none->accepted());
    ASSERT_FALSE(none->has_hazard_per_hour()); // no provider: unknown, not "zero"

    b.service->SetHazardProvider([] { return -1.0; }); // provider with no data yet
    auto unknown = client->Offer("t2", spec, {}, &err);
    ASSERT_TRUE(unknown.has_value());
    ASSERT_FALSE(unknown->has_hazard_per_hour());

    b.service->SetHazardProvider([] { return 0.75; });
    auto known = client->Offer("t3", spec, {}, &err);
    ASSERT_TRUE(known.has_value());
    ASSERT_TRUE(known->has_hazard_per_hour());
    ASSERT_TRUE(Near(known->hazard_per_hour(), 0.75, 1e-12));
}
