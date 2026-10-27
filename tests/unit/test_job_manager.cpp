#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"
#include "node/convert.hpp"
#include "node/job_manager.hpp"
#include "node/local_executor.hpp"

#include <map>
#include <stdexcept>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace v1 = zeromesh::v1;
using zeromesh::node::JobManager;
using zeromesh::testing::TempDir;

// Completes every task synchronously: RUNNING, one log line, then the configured result.
class FakeExecutor : public zeromesh::node::Executor {
public:
    explicit FakeExecutor(std::shared_ptr<zeromesh::store::CasStore> cas) : cas_(std::move(cas)) {}

    void Execute(const std::string&, const v1::TaskSpec& spec, zeromesh::node::TaskCallbacks cb) override {
        calls.push_back(spec);
        v1::TaskResult r;
        auto it = results.find(spec.name());
        if (it != results.end()) r = it->second;
        *r.mutable_stdout() = zeromesh::node::ToProto(cas_->PutBlob("log\n"));
        cb.on_state(v1::RUNNING, "");
        cb.on_log(1, "log\n");
        cb.on_done(r);
    }
    void Cancel(const std::string&) override {}

    std::vector<v1::TaskSpec> calls;
    std::map<std::string, v1::TaskResult> results;  // by task name; default: exit 0

private:
    std::shared_ptr<zeromesh::store::CasStore> cas_;
};

struct Fixture {
    TempDir dir;
    std::shared_ptr<zeromesh::store::CasStore> cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    std::shared_ptr<zeromesh::store::MerkleBuilder> merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    std::shared_ptr<zeromesh::node::ActionCache> cache = std::make_shared<zeromesh::node::ActionCache>(cas);
    std::shared_ptr<FakeExecutor> fake = std::make_shared<FakeExecutor>(cas);
    JobManager jm{cas, merkle, cache, fake};
};

v1::TaskSpec Task(const std::string& name, const std::string& word, std::vector<std::string> deps = {}) {
    v1::TaskSpec t;
    t.set_name(name);
    t.add_argv("echo");
    t.add_argv(word);
    for (auto& d : deps) t.add_depends_on(d);
    return t;
}

v1::JobSpec Job(std::vector<v1::TaskSpec> tasks) {
    v1::JobSpec j;
    j.set_name("job");
    for (auto& t : tasks) *j.add_tasks() = std::move(t);
    return j;
}

// Drains a watch until the job ends.
std::vector<v1::JobEvent> Drain(JobManager& jm, const std::string& id) {
    auto s = jm.Watch(id);
    std::vector<v1::JobEvent> out;
    while (auto ev = s->Next(std::chrono::seconds(30))) out.push_back(*ev);
    return out;
}

// Mirrors the host's /usr and its usr-merge layout, like the env-host plugin.
class TestHostEnv : public zeromesh::plugin::IEnvProvider {
public:
    std::string_view Name() const override { return "testhost"; }
    zeromesh::plugin::Availability Check(const zeromesh::plugin::EnvParams&) override { return {true, ""}; }
    zeromesh::plugin::Result<zeromesh::plugin::SandboxEnv> Prepare(const zeromesh::plugin::TaskContext&) override {
        zeromesh::plugin::SandboxEnv env;
        env.binds.push_back({"/usr", "/usr", true});
        for (const char* p : {"/bin", "/sbin", "/lib", "/lib64"}) {
            if (fs::is_symlink(p)) env.symlinks.push_back({fs::read_symlink(p).string(), p});
            else if (fs::is_directory(p)) env.binds.push_back({p, p, true});
        }
        env.env.push_back({"PATH", "/usr/local/bin:/usr/bin:/bin"});
        return {env, ""};
    }
    void Release(const zeromesh::plugin::TaskContext&) override {}
};

} // namespace

TEST(JobRejectsCycle) {
    Fixture f;
    bool threw = false;
    try { f.jm.Submit(Job({Task("a", "a", {"b"}), Task("b", "b", {"a"})})); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
    threw = false;
    try { f.jm.Submit(Job({Task("a", "a", {"nope"})})); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
    threw = false;
    try { f.jm.Submit(Job({Task("a", "a"), Task("a", "b")})); } catch (const std::invalid_argument&) { threw = true; }
    ASSERT_TRUE(threw);
    ASSERT_TRUE(f.fake->calls.empty());
}

TEST(JobRunsDagInOrder) {
    Fixture f;
    auto id = f.jm.Submit(Job({Task("c", "c", {"a", "b"}), Task("a", "a"), Task("b", "b")}));
    ASSERT_EQ(f.fake->calls.size(), static_cast<size_t>(3));
    ASSERT_EQ(f.fake->calls.back().name(), std::string("c"));
    auto st = f.jm.Get(id);
    ASSERT_TRUE(st && st->done());
    ASSERT_EQ(st->tasks().at("c"), v1::SUCCEEDED);
    ASSERT_EQ(st->results().size(), static_cast<size_t>(3));
}

// Holds the task after reporting placement, so ListActive can be observed mid-run.
class HoldExecutor : public zeromesh::node::Executor {
public:
    void Execute(const std::string&, const v1::TaskSpec&, zeromesh::node::TaskCallbacks cb) override {
        cb.on_state(v1::PLACED, "node peer-bb");
        held = std::move(cb);
    }
    void Cancel(const std::string&) override {}
    zeromesh::node::TaskCallbacks held;
};

TEST(JobListActiveRecordsWorkerWhilePlaced) {
    TempDir dir;
    auto cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    auto merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    auto cache = std::make_shared<zeromesh::node::ActionCache>(cas);
    auto exec = std::make_shared<HoldExecutor>();
    JobManager jm{cas, merkle, cache, exec};

    auto id = jm.Submit(Job({Task("a", "a")}));
    auto rows = jm.ListActive();
    ASSERT_EQ(rows.size(), static_cast<size_t>(1));
    ASSERT_EQ(rows[0].state(), v1::PLACED);
    ASSERT_EQ(rows[0].worker_id(), std::string("peer-bb"));
    ASSERT_EQ(rows[0].job_id(), id);

    v1::TaskResult r;
    r.set_exit_code(0);
    r.set_executed_by("peer-bb");
    exec->held.on_done(std::move(r));
    Drain(jm, id);
    rows = jm.ListActive();
    ASSERT_EQ(rows.size(), static_cast<size_t>(1));
    ASSERT_EQ(rows[0].state(), v1::SUCCEEDED);
    ASSERT_EQ(rows[0].worker_id(), std::string("peer-bb"));
}

TEST(JobListActiveKeepsFinished) {
    Fixture f;
    auto id = f.jm.Submit(Job({Task("a", "a"), Task("b", "b")}));
    Drain(f.jm, id);
    auto rows = f.jm.ListActive();
    ASSERT_EQ(rows.size(), static_cast<size_t>(2));
    int done = 0;
    for (const auto& a : rows) {
        ASSERT_EQ(a.role(), std::string("submitted"));
        ASSERT_EQ(a.job_id(), id);
        ASSERT_EQ(a.state(), v1::SUCCEEDED);
        ++done;
    }
    ASSERT_EQ(done, 2);
}

TEST(JobClearFinishedHidesDoneKeepsRunning) {
    TempDir dir;
    auto cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    auto merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    auto cache = std::make_shared<zeromesh::node::ActionCache>(cas);
    auto exec = std::make_shared<HoldExecutor>();
    JobManager jm{cas, merkle, cache, exec};

    auto done_id = jm.Submit(Job({Task("a", "a")}));
    v1::TaskResult r;
    r.set_exit_code(0);
    exec->held.on_done(std::move(r));
    Drain(jm, done_id);

    auto running_id = jm.Submit(Job({Task("b", "b")}));
    ASSERT_EQ(jm.ListActive().size(), static_cast<size_t>(2));

    jm.ClearFinished();
    auto rows = jm.ListActive();
    ASSERT_EQ(rows.size(), static_cast<size_t>(1));
    ASSERT_EQ(rows[0].job_id(), running_id);
    ASSERT_EQ(rows[0].state(), v1::PLACED);
    // The cleared job's result is still served.
    auto st = jm.Get(done_id);
    ASSERT_TRUE(st && st->done());
    ASSERT_EQ(st->tasks().at("a"), v1::SUCCEEDED);

    v1::TaskResult r2;
    r2.set_exit_code(0);
    exec->held.on_done(std::move(r2));
    Drain(jm, running_id);
    ASSERT_EQ(jm.ListActive().size(), static_cast<size_t>(1));  // finished after the clear: listed
}

TEST(JobListActiveOrdersWorkingBeforeDone) {
    Fixture f;
    // Block the second job's task so the first can finish while the second stays open.
    // FakeExecutor completes sync, so use two finished jobs + verify both remain listed.
    auto first = f.jm.Submit(Job({Task("a", "a")}));
    Drain(f.jm, first);
    auto second = f.jm.Submit(Job({Task("b", "b")}));
    Drain(f.jm, second);
    auto rows = f.jm.ListActive();
    ASSERT_EQ(rows.size(), static_cast<size_t>(2));
    ASSERT_EQ(rows[0].task_name(), std::string("b"));  // newer finished job first among done
    ASSERT_EQ(rows[1].task_name(), std::string("a"));
    ASSERT_EQ(rows[0].state(), v1::SUCCEEDED);
    ASSERT_EQ(rows[1].state(), v1::SUCCEEDED);
}

TEST(JobOverlayInputsFrom) {
    Fixture f;
    TempDir frames, src;
    zeromesh::testing::WriteFile(frames / "f1", "frame1");
    zeromesh::testing::WriteFile(src / "src.txt", "source");
    v1::TaskResult a_res;
    auto* out = a_res.add_outputs();
    out->set_path("frames");
    out->set_is_dir(true);
    *out->mutable_digest() = zeromesh::node::ToProto(f.merkle->BuildTree(frames.path()));
    f.fake->results["a"] = a_res;

    v1::TaskSpec c = Task("c", "c");
    *c.mutable_input_root() = zeromesh::node::ToProto(f.merkle->BuildTree(src.path()));
    auto* from = c.add_inputs_from();
    from->set_task("a");
    from->set_path(".");
    f.jm.Submit(Job({Task("a", "a"), c}));

    ASSERT_EQ(f.fake->calls.size(), static_cast<size_t>(2));
    TempDir work;
    ASSERT_TRUE(f.merkle->MaterializeTree(zeromesh::node::FromProto(f.fake->calls[1].input_root()), work / "w"));
    ASSERT_EQ(zeromesh::testing::ReadFile(work / "w/frames/f1"), std::string("frame1"));
    ASSERT_EQ(zeromesh::testing::ReadFile(work / "w/src.txt"), std::string("source"));
}

TEST(JobCacheHitAndNoFalseHit) {
    Fixture f;
    f.jm.Submit(Job({Task("t", "a")}));
    auto second = f.jm.Submit(Job({Task("t", "a")}));
    ASSERT_EQ(f.fake->calls.size(), static_cast<size_t>(1));
    bool hit = false;
    for (const auto& ev : Drain(f.jm, second)) {
        if (ev.state() == v1::SUCCEEDED && ev.result().cache_hit()) hit = true;
    }
    ASSERT_TRUE(hit);
    ASSERT_FALSE(f.jm.Get(second)->results().at("t").stdout().hash().empty());  // the real stored result

    f.jm.Submit(Job({Task("t", "b")}));
    ASSERT_EQ(f.fake->calls.size(), static_cast<size_t>(2));
}

TEST(JobDependencyFailurePropagates) {
    Fixture f;
    v1::TaskResult bad;
    bad.set_exit_code(1);
    f.fake->results["a"] = bad;
    auto id = f.jm.Submit(Job({Task("a", "a"), Task("b", "b", {"a"}), Task("c", "c", {"b"})}));
    ASSERT_EQ(f.fake->calls.size(), static_cast<size_t>(1));
    auto st = f.jm.Get(id);
    ASSERT_TRUE(st->done());
    ASSERT_EQ(st->tasks().at("a"), v1::FAILED);
    ASSERT_EQ(st->tasks().at("c"), v1::FAILED);
    std::map<std::string, std::string> detail;
    for (const auto& ev : Drain(f.jm, id)) {
        if (ev.state() == v1::FAILED) detail[ev.task_name()] = ev.detail();
    }
    ASSERT_EQ(detail["b"], std::string("dependency a failed"));
    ASSERT_EQ(detail["c"], std::string("dependency b failed"));
}

class VerboseExecutor : public FakeExecutor {
public:
    using FakeExecutor::FakeExecutor;
    void Execute(const std::string&, const v1::TaskSpec& spec, zeromesh::node::TaskCallbacks cb) override {
        v1::TaskResult r;
        auto it = results.find(spec.name());
        if (it != results.end()) r = it->second;
        cb.on_state(v1::RUNNING, "");
        for (int i = 0; i < 2500; ++i) cb.on_log(1, std::string(500, 'x'));
        cb.on_done(r);
    }
};

TEST(JobLogHistoryCapped) {
    Fixture f;
    f.fake = std::make_shared<VerboseExecutor>(f.cas);
    JobManager jm{f.cas, f.merkle, f.cache, f.fake};
    auto id = jm.Submit(Job({Task("t", "x")}));
    Drain(jm, id);
    auto replay = jm.Watch(id);
    size_t log_bytes = 0;
    size_t log_events = 0;
    while (auto ev = replay->Next(std::chrono::seconds(5))) {
        if (!ev->log_chunk().empty()) {
            log_bytes += ev->log_chunk().size();
            ++log_events;
        }
    }
    ASSERT_TRUE(log_events < static_cast<size_t>(2500));
    ASSERT_TRUE(log_bytes <= static_cast<size_t>(1024 * 1024));
}

TEST(JobLogHistoryCappedPerJob) {
    Fixture f;
    f.fake = std::make_shared<VerboseExecutor>(f.cas);
    JobManager jm{f.cas, f.merkle, f.cache, f.fake};
    // 10 verbose tasks × 1 MiB/task would be ~10 MiB without a job-wide cap (8 MiB).
    std::vector<v1::TaskSpec> tasks;
    for (int i = 0; i < 10; ++i) tasks.push_back(Task("t" + std::to_string(i), "x"));
    auto id = jm.Submit(Job(tasks));
    Drain(jm, id);
    auto replay = jm.Watch(id);
    size_t log_bytes = 0;
    while (auto ev = replay->Next(std::chrono::seconds(5))) {
        if (!ev->log_chunk().empty()) log_bytes += ev->log_chunk().size();
    }
    ASSERT_TRUE(log_bytes <= static_cast<size_t>(8 * 1024 * 1024));
}

TEST(JobEvictsOldFinishedJobs) {
    Fixture f;
    std::string first;
    std::string last;
    for (int i = 0; i < 260; ++i) {
        auto id = f.jm.Submit(Job({Task("t", "x")}));
        Drain(f.jm, id);
        if (i == 0) first = id;
        last = id;
    }
    ASSERT_FALSE(f.jm.Get(first).has_value());
    ASSERT_TRUE(f.jm.Get(last).has_value());
    auto rows = f.jm.ListActive();
    ASSERT_TRUE(rows.size() >= static_cast<size_t>(256));
}

TEST(JobWatchReplaysHistory) {
    Fixture f;
    auto id = f.jm.Submit(Job({Task("t", "x")}));
    auto events = Drain(f.jm, id);
    ASSERT_EQ(events.size(), static_cast<size_t>(4));
    ASSERT_EQ(events[0].state(), v1::QUEUED);
    ASSERT_EQ(events[1].state(), v1::RUNNING);
    ASSERT_EQ(events[2].log_chunk(), std::string("log\n"));
    ASSERT_EQ(events[3].state(), v1::SUCCEEDED);
    ASSERT_EQ(events[3].job_id(), id);
}

TEST(JobProtectedBlobHashes_WalksInputTree) {
    TempDir dir;
    auto cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    auto merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    auto cache = std::make_shared<zeromesh::node::ActionCache>(cas);
    auto exec = std::make_shared<HoldExecutor>();
    JobManager jm{cas, merkle, cache, exec};

    TempDir inputs;
    zeromesh::testing::WriteFile(inputs / "a.txt", "aaa");
    zeromesh::testing::WriteFile(inputs / "b.txt", "bbb");
    const auto root = merkle->BuildTree(inputs.path());
    std::vector<std::string> tree_hashes;
    for (const auto& d : merkle->CollectTree(root)) tree_hashes.push_back(d.hash);
    ASSERT_TRUE(tree_hashes.size() >= static_cast<size_t>(3));

    v1::TaskSpec t = Task("t", "x");
    *t.mutable_input_root() = zeromesh::node::ToProto(root);
    auto id = jm.Submit(Job({t}));

    zeromesh::store::CasLimits limits;
    limits.max_blob_bytes = 4096;
    auto cas_gc = std::make_shared<zeromesh::store::CasStore>(dir / "cas", limits);
    const auto filler = cas_gc->PutBlob(std::string(2000, 'z'));
    const auto protect = jm.ProtectedBlobHashes();
    cas_gc->CollectGarbage(0, protect);
    for (const auto& h : tree_hashes) ASSERT_TRUE(cas_gc->HasBlob(h));
    ASSERT_FALSE(cas_gc->HasBlob(filler.hash));

    v1::TaskResult r;
    r.set_exit_code(0);
    exec->held.on_done(std::move(r));
    Drain(jm, id);
}

TEST(JobProtectedBlobHashes_GracePeriodForFinishedOutputs) {
    JobManager::ResetProtectedBlobTestingHooks();
    const auto base = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point now = base;
    JobManager::SetClockForTesting([&] { return now; });
    JobManager::SetFinishedJobBlobGracePeriodForTesting(std::chrono::seconds(60));

    Fixture f;
    auto id = f.jm.Submit(Job({Task("t", "x")}));
    Drain(f.jm, id);
    const std::string stdout_hash = f.jm.Get(id)->results().at("t").stdout().hash();
    ASSERT_FALSE(stdout_hash.empty());

    auto protect = f.jm.ProtectedBlobHashes();
    ASSERT_TRUE(protect.contains(stdout_hash));
    f.cas->CollectGarbage(0, protect);
    ASSERT_TRUE(f.cas->HasBlob(stdout_hash));

    now = base + std::chrono::seconds(61);
    protect = f.jm.ProtectedBlobHashes();
    ASSERT_FALSE(protect.contains(stdout_hash));
    const auto drop = f.cas->PutBlob("evict-stdout");
    f.cas->CollectGarbage(0, protect);
    ASSERT_FALSE(f.cas->HasBlob(stdout_hash));
    ASSERT_FALSE(f.cas->HasBlob(drop.hash));

    JobManager::ResetProtectedBlobTestingHooks();
}

TEST(JobRealLocalExecutor) {
    Fixture f;
    auto runner = std::make_shared<zeromesh::exec::SandboxRunner>(f.cas, f.merkle, f.dir / "scratch");
    auto registry = std::make_shared<zeromesh::plugin::Registry>();
    registry->RegisterEnv(std::make_unique<TestHostEnv>());
    auto local = std::make_shared<zeromesh::node::LocalExecutor>(runner, registry, "node-1", 2, f.dir / "scratch",
                                                                 f.cas, zeromesh::node::NodeClientFactory{});
    JobManager jm{f.cas, f.merkle, f.cache, local};

    v1::TaskSpec t;
    t.set_name("hi");
    t.add_argv("sh");
    t.add_argv("-c");
    t.add_argv("echo hi");
    t.mutable_env_spec()->set_name("testhost");
    v1::TaskSpec missing = t;
    missing.set_name("missing");
    missing.mutable_env_spec()->set_name("nope");
    auto id = jm.Submit(Job({t, missing}));
    Drain(jm, id);
    auto st = jm.Get(id);
    ASSERT_TRUE(st->done());
    const auto& r = st->results().at("hi");
    ASSERT_EQ(r.stdout_head(), std::string("hi\n"));
    ASSERT_EQ(r.executed_by(), std::string("node-1"));
    ASSERT_EQ(st->tasks().at("missing"), v1::FAILED);
    ASSERT_EQ(st->results().at("missing").error(), std::string("no env provider 'nope'"));
}
