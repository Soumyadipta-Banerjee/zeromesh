// Partial commit (v2 blueprint, claim 3): a task commits finished units as it goes
// (atomic rename into $ZM_UNITS_DIR); the sandbox streams each one into the CAS, the
// requester keeps them as partial action-cache entries, and a retry or re-run of the same
// task starts with those units already in place so only the missing ones run again.

#include "tests/unit/node_fixture.hpp"

#include "exec/sandbox_runner.hpp"
#include "exec/units.hpp"
#include "node/action_cache.hpp"
#include "node/convert.hpp"
#include "node/job_manager.hpp"
#include "node/task_key.hpp"
#include "plugins/env-host/env_host.hpp"
#include "sched/placer.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace v1 = zeromesh::v1;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;

namespace {

using Clock = std::chrono::steady_clock;

void Expect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

struct RunnerFixture {
    TempDir dir;
    std::shared_ptr<zeromesh::store::CasStore> cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    std::shared_ptr<zeromesh::store::MerkleBuilder> merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    zeromesh::exec::SandboxRunner runner{cas, merkle, dir / "scratch"};

    zeromesh::exec::RunResult Sh(const std::string& script, zeromesh::exec::RunSpec spec = {}) {
        spec.argv = {"sh", "-c", script};
        spec.sandbox_env = *zeromesh::envhost::Make()->Prepare({}).value;
        return runner.Run(spec, {}, std::stop_token{});
    }
};

struct Requester {
    std::shared_ptr<zeromesh::sched::Placer> placer;
    std::shared_ptr<zeromesh::node::ActionCache> cache;
    std::unique_ptr<zeromesh::node::JobManager> jm;

    explicit Requester(TestNodeFull& self) {
        auto factory = [&self](const zeromesh::net::Peer& p) {
            return std::make_shared<zeromesh::node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port),
                                                                self.id, self.pins, p.id);
        };
        placer = std::make_shared<zeromesh::sched::Placer>(self.executor, self.peers, factory, self.cas, self.merkle,
                                                           self.node_id, [&self] { return self.telemetry; });
        cache = std::make_shared<zeromesh::node::ActionCache>(self.cas);
        jm = std::make_unique<zeromesh::node::JobManager>(self.cas, self.merkle, cache, placer);
    }
};

// Six units, 0.4 s each. A unit already in $ZM_UNITS_DIR is read, not recomputed.
// ran.txt lists the units this attempt computed; all.txt is every unit's content in order.
const char* kUnitScript =
    "for i in 1 2 3 4 5 6; do "
    "  f=\"$ZM_UNITS_DIR/u$i\"; "
    "  if [ -e \"$f\" ]; then cat \"$f\" >> all.txt; continue; fi; "
    "  sleep 0.4; "
    "  printf 'unit %s\\n' $i > \"$ZM_UNITS_DIR/.tmp$i\"; "
    "  mv \"$ZM_UNITS_DIR/.tmp$i\" \"$f\"; "
    "  echo \"ran u$i\" >> ran.txt; "
    "  cat \"$f\" >> all.txt; "
    "done";

v1::JobSpec UnitJob(int timeout_s = 0) {
    v1::JobSpec job;
    job.set_name("units");
    auto* t = job.add_tasks();
    t->set_name("t");
    t->add_argv("sh");
    t->add_argv("-c");
    t->add_argv(kUnitScript);
    t->add_outputs("ran.txt");
    t->add_outputs("all.txt");
    t->mutable_policy()->set_may_run_local(false);
    if (timeout_s > 0) t->mutable_policy()->set_timeout_s(timeout_s);
    return job;
}

void WaitDone(zeromesh::node::JobManager& jm, const std::string& id, std::chrono::seconds limit = 60s) {
    const auto deadline = Clock::now() + limit;
    while (Clock::now() < deadline) {
        if (jm.Get(id)->done()) return;
        std::this_thread::sleep_for(20ms);
    }
    throw std::runtime_error("job " + id + " did not finish");
}

// Content of the named output of task "t", pulled into the requester's CAS.
std::string OutputText(TestNodeFull& a, const v1::TaskResult& r, const std::string& path) {
    for (const auto& o : r.outputs()) {
        if (o.path() == path) {
            auto blob = a.cas->GetBlob(o.digest().hash());
            Expect(blob.has_value(), "output " + path + " is not in the requester's CAS");
            return *blob;
        }
    }
    return "<no output " + path + ">";
}

bool CgroupFrozen(const std::filesystem::path& dir) {
    std::ifstream in(dir / "cgroup.events");
    std::string k;
    int v = 0;
    while (in >> k >> v) {
        if (k == "frozen") return v == 1;
    }
    return false;
}

} // namespace

// ---- unit ids -------------------------------------------------------------------------------

TEST(UnitIdsAreLimitedToSafeFileNames) {
    using zeromesh::exec::IsValidUnitId;
    ASSERT_TRUE(IsValidUnitId("u1"));
    ASSERT_TRUE(IsValidUnitId("frame_0042.exr"));
    ASSERT_TRUE(IsValidUnitId("case-7"));
    ASSERT_FALSE(IsValidUnitId(""));
    ASSERT_FALSE(IsValidUnitId(".tmp1"));         // dot files are the atomic-write staging area
    ASSERT_FALSE(IsValidUnitId("a/b"));
    ASSERT_FALSE(IsValidUnitId("../x"));
    ASSERT_FALSE(IsValidUnitId("sp ace"));
    ASSERT_FALSE(IsValidUnitId(std::string(129, 'x')));
    ASSERT_TRUE(IsValidUnitId(std::string(128, 'x')));
}

// ---- action cache: partial entries ----------------------------------------------------------

TEST(ActionCacheKeepsPartialUnitsUntilTheTaskSucceeds) {
    TempDir tmp;
    auto cas = std::make_shared<zeromesh::store::CasStore>(tmp / "cas");
    zeromesh::node::ActionCache cache(cas);
    const std::string key(64, 'a');

    ASSERT_TRUE(cache.GetUnits(key).empty());
    const auto d1 = cas->PutBlob("unit one");
    const auto d2 = cas->PutBlob("unit two");
    cache.PutUnit(key, "u1", zeromesh::node::ToProto(d1));
    cache.PutUnit(key, "u2", zeromesh::node::ToProto(d2));
    cache.PutUnit(key, "u1", zeromesh::node::ToProto(d1)); // committing twice is harmless

    auto units = cache.GetUnits(key);
    ASSERT_EQ(units.size(), static_cast<size_t>(2));
    ASSERT_EQ(units.at("u1").hash(), d1.hash);
    ASSERT_EQ(units.at("u2").hash(), d2.hash);

    // A partial entry is not a result: the normal lookup still misses.
    ASSERT_FALSE(cache.Get(key).has_value());
    // Other keys have their own units.
    ASSERT_TRUE(cache.GetUnits(std::string(64, 'b')).empty());

    // Success supersedes the partial entry.
    v1::TaskResult ok;
    ok.set_exit_code(0);
    cache.Put(key, ok);
    ASSERT_TRUE(cache.GetUnits(key).empty());
    ASSERT_TRUE(cache.Get(key).has_value());
}

TEST(ActionCacheDropsUnitsWhoseBlobIsGone) {
    TempDir tmp;
    auto cas = std::make_shared<zeromesh::store::CasStore>(tmp / "cas");
    zeromesh::node::ActionCache cache(cas);
    const std::string key(64, 'c');
    const auto kept = cas->PutBlob("kept");
    // A digest for a blob that was never stored.
    const auto ghost = zeromesh::store::CasStore::ComputeDigest("never stored");
    cache.PutUnit(key, "have", zeromesh::node::ToProto(kept));
    cache.PutUnit(key, "gone", zeromesh::node::ToProto(ghost));

    auto units = cache.GetUnits(key);
    ASSERT_EQ(units.size(), static_cast<size_t>(1));
    ASSERT_TRUE(units.contains("have"));

    cache.PruneStale(); // must not delete partial entries that are still usable
    ASSERT_EQ(cache.GetUnits(key).size(), static_cast<size_t>(1));
}

TEST(ActionCacheIgnoresInvalidUnitIds) {
    TempDir tmp;
    auto cas = std::make_shared<zeromesh::store::CasStore>(tmp / "cas");
    zeromesh::node::ActionCache cache(cas);
    const std::string key(64, 'd');
    const auto d = cas->PutBlob("x");
    cache.PutUnit(key, "../escape", zeromesh::node::ToProto(d));
    cache.PutUnit(key, ".hidden", zeromesh::node::ToProto(d));
    ASSERT_TRUE(cache.GetUnits(key).empty());
}

// ---- sandbox: units stream out as they are committed ----------------------------------------

TEST(SandboxStreamsCommittedUnitsWhileTheTaskRuns) {
    RunnerFixture f;
    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> got; // (unit id, blob content)
    std::vector<Clock::time_point> when;
    zeromesh::exec::RunSpec spec;
    spec.on_unit = [&](const std::string& id, const zeromesh::store::Digest& d) {
        std::lock_guard lock(mu);
        got.emplace_back(id, f.cas->GetBlob(d.hash).value_or("<missing>"));
        when.push_back(Clock::now());
    };
    const auto t0 = Clock::now();
    auto r = f.Sh("sleep 0.3; echo one > \"$ZM_UNITS_DIR/.tmp\"; mv \"$ZM_UNITS_DIR/.tmp\" \"$ZM_UNITS_DIR/u1\"; "
                  "sleep 0.5; echo two > \"$ZM_UNITS_DIR/.tmp\"; mv \"$ZM_UNITS_DIR/.tmp\" \"$ZM_UNITS_DIR/u2\"; "
                  "sleep 0.5",
                  spec);
    const auto t_end = Clock::now();
    ASSERT_EQ(r.error, std::string());
    ASSERT_EQ(r.exit_code, 0);
    std::lock_guard lock(mu);
    ASSERT_EQ(got.size(), static_cast<size_t>(2));
    ASSERT_EQ(got[0].first, std::string("u1"));
    ASSERT_EQ(got[0].second, std::string("one\n"));
    ASSERT_EQ(got[1].first, std::string("u2"));
    ASSERT_EQ(got[1].second, std::string("two\n"));
    // u1 was reported while the task was still running, not after it exited.
    Expect(when[0] < t_end - 700ms, "u1 was only reported at the end of the run");
    Expect(when[0] > t0, "clock sanity");
}

TEST(SandboxDoesNotReportUnitsThatArrivedWithTheInput) {
    RunnerFixture f;
    const auto old_unit = f.cas->PutBlob("from a previous attempt\n");
    zeromesh::exec::RunSpec spec;
    spec.input_root = f.merkle->Overlay(zeromesh::store::Digest{}, ".zm_units/u0", old_unit, false);
    std::vector<std::string> reported;
    spec.on_unit = [&](const std::string& id, const zeromesh::store::Digest&) { reported.push_back(id); };
    auto r = f.Sh("cat \"$ZM_UNITS_DIR/u0\"; echo n > \"$ZM_UNITS_DIR/.t\"; mv \"$ZM_UNITS_DIR/.t\" \"$ZM_UNITS_DIR/u1\"",
                  spec);
    ASSERT_EQ(r.error, std::string());
    ASSERT_EQ(r.exit_code, 0);
    ASSERT_EQ(r.stdout_head, std::string("from a previous attempt\n")); // the task sees the old unit
    ASSERT_EQ(reported.size(), static_cast<size_t>(1));
    ASSERT_EQ(reported[0], std::string("u1")); // only the new one is reported
}

TEST(SandboxKeepsUnitFilesOutOfCaptureChanged) {
    RunnerFixture f;
    zeromesh::exec::RunSpec spec;
    spec.capture_changed = true;
    spec.on_unit = [](const std::string&, const zeromesh::store::Digest&) {};
    auto r = f.Sh("echo r > result.txt; echo n > \"$ZM_UNITS_DIR/.t\"; mv \"$ZM_UNITS_DIR/.t\" \"$ZM_UNITS_DIR/u1\"",
                  spec);
    ASSERT_EQ(r.exit_code, 0);
    bool result_txt = false;
    for (const auto& o : r.outputs) {
        ASSERT_FALSE(o.path.rfind(".zm_units", 0) == 0); // a resumed run must look like a full one
        if (o.path == "result.txt") result_txt = true;
    }
    ASSERT_TRUE(result_txt);
}

TEST(SandboxIgnoresUnitFilesWithoutACallback) {
    RunnerFixture f;
    auto r = f.Sh("echo n > \"$ZM_UNITS_DIR/u1\"; echo ok");
    ASSERT_EQ(r.error, std::string());
    ASSERT_EQ(r.stdout_head, std::string("ok\n"));
}

TEST(SandboxFrozenTimeDoesNotCountAgainstTheTimeout) {
    if (!zeromesh::exec::DelegatedCgroupParent()) return; // needs a delegated cgroup v2 subtree
    RunnerFixture f;
    zeromesh::exec::RunSpec spec;
    spec.task_tag = "frozen-tag";
    spec.timeout = 3s;
    spec.cores = 1;
    zeromesh::exec::RunResult r;
    std::thread th([&] { r = f.Sh("sleep 2; echo finished", spec); });

    std::optional<std::filesystem::path> dir;
    for (int i = 0; i < 100 && !dir; ++i) {
        dir = f.runner.CgroupFor("frozen-tag");
        if (!dir) std::this_thread::sleep_for(20ms);
    }
    if (!dir) {
        th.join();
        return; // no task cgroup on this host
    }
    std::this_thread::sleep_for(300ms);
    Expect(f.runner.SetTaskFrozen("frozen-tag", true), "freeze failed");
    std::this_thread::sleep_for(3s); // longer than the whole timeout
    ASSERT_TRUE(CgroupFrozen(*dir));
    Expect(f.runner.SetTaskFrozen("frozen-tag", false), "thaw failed");
    th.join();
    ASSERT_FALSE(r.timed_out);
    ASSERT_EQ(r.exit_code, 0);
    ASSERT_EQ(r.stdout_head, std::string("finished\n"));
}

// ---- job manager + local executor: the whole chain on one node ------------------------------

TEST(LocalRunKeepsUnitsOfAFailedTaskAndResumesFromThem) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key);
    a.Start();
    auto cache = std::make_shared<zeromesh::node::ActionCache>(a.cas);
    zeromesh::node::JobManager jm(a.cas, a.merkle, cache, a.executor);

    auto submit = [&] {
        v1::JobSpec job;
        job.set_name("resume");
        auto* t = job.add_tasks();
        t->set_name("t");
        t->add_argv("sh");
        t->add_argv("-c");
        // First attempt: commit u1, then fail. A later attempt finds u1 already in place.
        t->add_argv("if [ -e \"$ZM_UNITS_DIR/u1\" ]; then echo had-u1; exit 0; fi; "
                    "echo one > \"$ZM_UNITS_DIR/.t\"; mv \"$ZM_UNITS_DIR/.t\" \"$ZM_UNITS_DIR/u1\"; exit 1");
        return jm.Submit(job);
    };

    const std::string first = submit();
    WaitDone(jm, first);
    auto st1 = jm.Get(first);
    ASSERT_EQ(st1->tasks().at("t"), v1::FAILED);
    const std::string task_key = st1->results().at("t").task_key();
    ASSERT_FALSE(task_key.empty());
    auto units = cache->GetUnits(task_key);
    ASSERT_EQ(units.size(), static_cast<size_t>(1));
    ASSERT_TRUE(units.contains("u1"));

    const std::string second = submit(); // identical task, so the same key
    WaitDone(jm, second);
    auto st2 = jm.Get(second);
    Expect(st2->tasks().at("t") == v1::SUCCEEDED, st2->results().at("t").error());
    ASSERT_EQ(st2->results().at("t").task_key(), task_key); // resuming must not change the identity
    ASSERT_EQ(st2->results().at("t").stdout_head(), std::string("had-u1\n"));
    ASSERT_TRUE(cache->GetUnits(task_key).empty()); // success cleared the partial entry
}

// ---- the end-to-end case: killed mid-way, retried, only the missing units re-run ------------

TEST(PartialCommitRetryRunsOnlyTheMissingUnits) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    a.peers->Upsert(b.AsPeer()); // only b is known at first, so b takes the task
    Requester req(a);

    const std::string id = req.jm->Submit(UnitJob());
    auto stream = req.jm->Watch(id);

    // Let b commit a few units.
    std::set<std::string> committed;
    const auto commit_deadline = Clock::now() + 30s;
    while (committed.size() < 3 && Clock::now() < commit_deadline) {
        auto ev = stream->Next(100ms);
        if (ev && ev->has_unit_committed()) committed.insert(ev->unit_committed().unit_id());
    }
    Expect(committed.size() >= 3, "fewer than 3 units were committed on the first donor");

    // b leaves (plain drain: the run is cancelled and re-dispatched); c is available.
    a.peers->Upsert(c.AsPeer());
    a.peers->MarkDraining("node-b", "battery");

    WaitDone(*req.jm, id, 90s);
    auto st = req.jm->Get(id);
    ASSERT_TRUE(st->done());
    const v1::TaskResult& res = st->results().at("t");
    Expect(st->tasks().at("t") == v1::SUCCEEDED, res.error());
    ASSERT_EQ(res.executed_by(), std::string("node-c"));

    // c computed only what b had not committed: none of u1..u3, and it did finish u6.
    const std::string ran = OutputText(a, res, "ran.txt");
    for (const char* done_on_b : {"u1", "u2", "u3"}) {
        Expect(ran.find(std::string("ran ") + done_on_b) == std::string::npos,
               std::string(done_on_b) + " ran again on the second donor:\n" + ran);
    }
    Expect(ran.find("ran u6") != std::string::npos, "the last unit never ran on the second donor:\n" + ran);

    // Yet the final result holds all six units, the first ones carried over from b.
    ASSERT_EQ(OutputText(a, res, "all.txt"),
              std::string("unit 1\nunit 2\nunit 3\nunit 4\nunit 5\nunit 6\n"));

    // The job saw a drain retry, and success cleared the partial entry.
    bool saw_retry = false;
    while (auto ev = stream->Next(10ms)) {
        if (ev->state() == v1::QUEUED && ev->detail().find("retry 1/") == 0) saw_retry = true;
    }
    ASSERT_TRUE(saw_retry);
    ASSERT_TRUE(req.cache->GetUnits(res.task_key()).empty());
}

TEST(RerunAfterATimeoutRunsOnlyTheMissingUnits) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    // First run: a 1 s timeout cuts the task off after the first couple of units.
    const std::string first = req.jm->Submit(UnitJob(/*timeout_s=*/1));
    WaitDone(*req.jm, first);
    auto st1 = req.jm->Get(first);
    ASSERT_EQ(st1->tasks().at("t"), v1::FAILED);
    const std::string task_key = st1->results().at("t").task_key();
    const auto saved = req.cache->GetUnits(task_key);
    Expect(!saved.empty(), "no unit survived the timed-out run");
    Expect(saved.size() < 6, "the timeout never cut the run short");

    // Second run: same task (the timeout is not part of its identity), no cut-off.
    const std::string second = req.jm->Submit(UnitJob(/*timeout_s=*/60));
    WaitDone(*req.jm, second, 90s);
    auto st2 = req.jm->Get(second);
    const v1::TaskResult& res = st2->results().at("t");
    Expect(st2->tasks().at("t") == v1::SUCCEEDED, res.error());
    ASSERT_EQ(res.task_key(), task_key);

    const std::string ran = OutputText(a, res, "ran.txt");
    for (const auto& [unit, _] : saved) {
        Expect(ran.find("ran " + unit) == std::string::npos,
               unit + " was recomputed although it was saved:\n" + ran);
    }
    ASSERT_EQ(OutputText(a, res, "all.txt"),
              std::string("unit 1\nunit 2\nunit 3\nunit 4\nunit 5\nunit 6\n"));
}
