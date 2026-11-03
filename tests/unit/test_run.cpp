// The run engine and what it is built from: item keys and footprints, the item cache, learned
// resources, environment checks and output placement. The engine runs against a scripted
// executor here (every behaviour is deterministic) and, at the end, against a real placer and
// donors.

#include "tests/unit/node_fixture.hpp"

#include "node/convert.hpp"
#include "node/env_check.hpp"
#include "run/item_cache.hpp"
#include "run/item_key.hpp"
#include "run/place.hpp"
#include "run/resources.hpp"
#include "run/run_engine.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace v1 = zeromesh::v1;
namespace run = zeromesh::run;
namespace store = zeromesh::store;
using zeromesh::testing::ReadFile;
using zeromesh::testing::TempDir;
using zeromesh::testing::WriteFile;
using namespace std::chrono_literals;

namespace {

void Expect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

struct Stores {
    TempDir dir;
    std::shared_ptr<store::CasStore> cas = std::make_shared<store::CasStore>(dir / "cas");
    std::shared_ptr<store::MerkleBuilder> merkle = std::make_shared<store::MerkleBuilder>(cas);
};

v1::Item MakeItem(const std::string& id, std::vector<std::string> argv) {
    v1::Item it;
    it.set_id(id);
    for (auto& a : argv) it.add_argv(a);
    return it;
}

// An executor that answers every batch with a script: per item id, what happens on which attempt.
class ScriptedExecutor final : public zeromesh::node::Executor {
public:
    struct Answer {
        v1::FailureKind item_failure{v1::FAILURE_NONE}; // NONE: success with the declared outputs
        int exit_code{0};
        bool donor_lost{false};                   // the batch ends without this item
        v1::FailureKind batch_failure{v1::FAILURE_DONOR_LEFT}; // with donor_lost
        std::vector<std::string> footprint{};
        std::string output_text = "out";
        int64_t peak_mem{0};
        int64_t wall_ms{10};
        bool oom_at_limit{false}; // FAILURE_OOM: the batch's own limit, not the node's budget
        int64_t budget{0};        // FAILURE_OOM not at the limit: what work on that node may use
    };
    // Answers for an item id, used one per attempt; the last repeats.
    std::map<std::string, std::deque<Answer>> script;
    std::mutex mu;
    std::vector<v1::BatchSpec> batches;
    std::vector<std::set<std::string>> excludes;
    std::shared_ptr<store::CasStore> cas;
    std::string node = "donor-1";
    std::atomic<int> active{0};
    int peak_active{0};

    void Execute(const std::string& batch_id, const v1::BatchSpec& spec, std::set<std::string> exclude,
                 zeromesh::node::BatchCallbacks cb) override {
        {
            std::lock_guard lock(mu);
            batches.push_back(spec);
            excludes.push_back(exclude);
            peak_active = std::max(peak_active, ++active);
        }
        std::thread([this, batch_id, spec, cb] {
            std::this_thread::sleep_for(20ms);
            if (cb.on_placed) cb.on_placed(node);
            zeromesh::node::BatchDone done;
            done.node = node;
            for (const auto& bi : spec.items()) {
                Answer a;
                {
                    std::lock_guard lock(mu);
                    auto& q = script[bi.id()];
                    if (!q.empty()) {
                        a = q.front();
                        if (q.size() > 1) q.pop_front();
                    }
                }
                if (cb.on_started) cb.on_started(bi.id());
                if (a.donor_lost) {
                    done.failure = a.batch_failure;
                    done.message = "scripted loss";
                    break;
                }
                v1::ItemOutcome o;
                o.set_id(bi.id());
                o.set_failure(a.item_failure);
                o.set_exit_code(a.item_failure == v1::FAILURE_EXIT ? a.exit_code : 0);
                o.set_message(a.item_failure == v1::FAILURE_NONE ? "" : "scripted failure");
                o.set_wall_ms(a.wall_ms);
                o.set_peak_mem(a.peak_mem);
                o.set_oom_at_limit(a.oom_at_limit);
                o.set_budget(a.budget);
                for (const auto& f : a.footprint) o.add_footprint(f);
                if (a.item_failure == v1::FAILURE_NONE) {
                    for (const auto& path : bi.outputs()) {
                        auto* f = o.add_outputs();
                        f->set_path(path);
                        *f->mutable_digest() = zeromesh::node::ToProto(cas->PutBlob(a.output_text + ":" + bi.id()));
                    }
                }
                *o.mutable_stdout() = zeromesh::node::ToProto(cas->PutBlob("log of " + bi.id()));
                if (cb.on_item) cb.on_item(o, node);
            }
            --active;
            if (cb.on_done) cb.on_done(done);
        }).detach();
    }
    void Cancel(const std::string&) override {}
};

struct Engine {
    Stores s;
    std::shared_ptr<ScriptedExecutor> exec = std::make_shared<ScriptedExecutor>();
    std::shared_ptr<run::ItemCache> cache = std::make_shared<run::ItemCache>(s.cas, s.merkle);
    std::shared_ptr<run::ResourceModel> resources = std::make_shared<run::ResourceModel>();
    int slots = 4;
    std::unique_ptr<run::RunEngine> engine;

    explicit Engine(std::shared_ptr<run::Team> team = nullptr, std::chrono::milliseconds session_grace = 1min) {
        exec->cas = s.cas;
        run::EngineDeps d;
        d.cas = s.cas;
        d.merkle = s.merkle;
        d.cache = cache;
        d.executor = exec;
        d.resources = resources;
        d.team = std::move(team);
        d.slots = [this](const v1::Resources&, v1::Where) {
            return zeromesh::node::Slots{slots, std::max(slots - exec->active.load(), 0)};
        };
        d.hostname = [](const std::string& id) { return "host-" + id; };
        d.runs_dir = s.dir / "runs";
        d.node_id = "requester";
        d.arch = "x86_64";
        d.session_grace = session_grace;
        engine = std::make_unique<run::RunEngine>(std::move(d));
    }

    // A run over `workspace` (a directory) whose default stage runs anywhere.
    v1::RunSpec Spec(const std::filesystem::path& workspace) {
        v1::RunSpec spec;
        spec.set_name("test");
        *spec.mutable_workspace() = zeromesh::node::ToProto(s.merkle->BuildTree(workspace));
        spec.add_stages();
        return spec;
    }

    v1::Report Wait(const std::string& id, std::chrono::seconds limit = 20s) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            auto st = engine->Status(id);
            if (st && st->done()) return *engine->Report(id);
            std::this_thread::sleep_for(20ms);
        }
        throw std::runtime_error("run " + id + " did not finish");
    }
};

const v1::ItemResult& Result(const v1::Report& r, const std::string& id) {
    for (const auto& it : r.items()) {
        if (it.id() == id) return it;
    }
    throw std::runtime_error("no item " + id);
}

} // namespace

// ---- keys and footprints ----------------------------------------------------------------------

TEST(ItemKeyChangesWithTheCommandNotWithWhereOutputsGo) {
    v1::Stage stage;
    auto a = MakeItem("a", {"cc", "-c", "a.c"});
    auto b = a;
    b.add_outputs()->set_path("a.o");
    auto c = b;
    c.mutable_outputs(0)->set_dest("/tmp/elsewhere/a.o");
    const std::string ka = run::WeakKey(a, stage, "host", "x86_64");
    ASSERT_NE(ka, run::WeakKey(b, stage, "host", "x86_64"));
    ASSERT_EQ(run::WeakKey(b, stage, "host", "x86_64"), run::WeakKey(c, stage, "host", "x86_64"));
    ASSERT_NE(ka, run::WeakKey(a, stage, "tree:abc", "x86_64"));
    ASSERT_NE(ka, run::WeakKey(a, stage, "host", "aarch64"));
    auto d = a;
    d.set_id("other-id"); // the id is not part of what the command does
    ASSERT_EQ(ka, run::WeakKey(d, stage, "host", "x86_64"));
}

TEST(ItemKeyWholeInputsAndDepfileModes) {
    Stores s;
    TempDir ws;
    WriteFile(ws / "src/a.c", "int a;");
    WriteFile(ws / "include/a.h", "int x;");
    WriteFile(ws / "notes.txt", "n");
    auto root = s.merkle->BuildTree(ws.path());
    WriteFile(ws / "notes.txt", "changed notes");
    auto root2 = s.merkle->BuildTree(ws.path());

    // Whole: any change changes the key.
    auto whole = MakeItem("w", {"x"});
    ASSERT_NE(run::FullKey("w", run::Observe(*s.merkle, root, whole)),
              run::FullKey("w", run::Observe(*s.merkle, root2, whole)));
    // Inputs: only the declared paths count.
    auto inputs = MakeItem("i", {"x"});
    inputs.add_inputs("src");
    ASSERT_EQ(run::FullKey("i", run::Observe(*s.merkle, root, inputs)),
              run::FullKey("i", run::Observe(*s.merkle, root2, inputs)));
    // Depfile: the files it named, plus the listing of its search directories.
    auto cc = MakeItem("c", {"cc"});
    cc.set_cwd("src");
    cc.set_depfile("a.d");
    cc.add_search_dirs("../include");
    auto fp = run::Observe(*s.merkle, root, cc, {"src/a.c", "include/a.h"});
    ASSERT_EQ(fp.files_size(), 2);
    ASSERT_EQ(fp.listings_size(), 1);
    ASSERT_EQ(fp.listings(0).path(), std::string("include"));
    ASSERT_EQ(run::FullKey("c", fp), run::FullKey("c", run::Revalue(*s.merkle, root2, fp)));
    // A new header that could shadow another changes the listing, and so the key.
    WriteFile(ws / "include/b.h", "int y;");
    auto root3 = s.merkle->BuildTree(ws.path());
    auto now = run::Revalue(*s.merkle, root3, fp);
    ASSERT_NE(run::FullKey("c", fp), run::FullKey("c", now));
    auto changed = run::Changed(*s.merkle, fp, now, 10);
    ASSERT_EQ(changed.size(), 1u);
    ASSERT_EQ(changed[0], std::string("include/"));
    // A changed header is named.
    WriteFile(ws / "include/a.h", "int x2;");
    auto root4 = s.merkle->BuildTree(ws.path());
    changed = run::Changed(*s.merkle, fp, run::Revalue(*s.merkle, root4, fp), 10);
    ASSERT_TRUE(std::find(changed.begin(), changed.end(), "include/a.h") != changed.end());
    // Paths that leave the workspace are refused.
    bool threw = false;
    try {
        run::WorkspacePath("src", "../../etc/passwd");
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

TEST(ItemCacheKeepsOnlyCompleteResultsAndNewestFootprints) {
    Stores s;
    run::ItemCache cache(s.cas, s.merkle);
    const auto blob = s.cas->PutBlob("object");
    v1::CachedItem item;
    auto* o = item.add_outputs();
    o->set_path("a.o");
    *o->mutable_digest() = zeromesh::node::ToProto(blob);
    const std::string key(64, 'a');
    cache.Put(key, item);
    ASSERT_TRUE(cache.Get(key).has_value());
    s.cas->CollectGarbage(0, {});
    ASSERT_FALSE(cache.Get(key).has_value()); // its output is gone
    cache.Prune();
    ASSERT_FALSE(s.cas->GetActionResult(key).has_value());

    const std::string weak(64, 'b');
    for (int i = 0; i < 12; ++i) {
        v1::Footprint fp;
        auto* f = fp.add_files();
        f->set_path("f" + std::to_string(i));
        cache.AddFootprint(weak, fp);
    }
    auto fps = cache.Footprints(weak);
    ASSERT_EQ(fps.size(), 8u);
    ASSERT_EQ(fps[0].files(0).path(), std::string("f11")); // newest first
    v1::Footprint again;
    again.add_files()->set_path("f5");
    cache.AddFootprint(weak, again); // same paths: replaces, does not duplicate
    fps = cache.Footprints(weak);
    ASSERT_EQ(fps[0].files(0).path(), std::string("f5"));
    int f5 = 0;
    for (const auto& f : fps) f5 += f.files(0).path() == "f5" ? 1 : 0;
    ASSERT_EQ(f5, 1);
    cache.Prune();
    ASSERT_EQ(cache.Footprints(weak).size(), 8u); // footprint lists are not results: kept
}

// ---- resources --------------------------------------------------------------------------------

TEST(ResourceModelLearnsPerKindAndSizeAndRisesAfterOom) {
    TempDir d;
    {
        run::ResourceModel m(d / "resources.tsv");
        ASSERT_FALSE(m.Seconds("cc", 1000).has_value());
        m.Record("cc", 1000, 2000, 100 << 20);   // a small unit
        m.Record("cc", 1e7, 30000, 800ll << 20); // a big one
        ASSERT_TRUE(*m.Seconds("cc", 1000) < 5.0);
        ASSERT_TRUE(*m.Seconds("cc", 1e7) > 20.0);
        // The largest peak seen, with the margin.
        ASSERT_EQ(*m.Memory("cc", 1000), 125ll << 20);
        ASSERT_EQ(*m.Memory("cc", 1e7), 1000ll << 20);
        // A small unit killed for memory holding 300 MiB needs at least that: reserved with the margin.
        m.NeedsAtLeast("cc", 1000, 300ll << 20);
        ASSERT_EQ(*m.Memory("cc", 1000), 375ll << 20);
    }
    run::ResourceModel reloaded(d / "resources.tsv");
    ASSERT_EQ(*reloaded.Memory("cc", 1000), 375ll << 20);
    ASSERT_TRUE(*reloaded.Seconds("cc", 1e7) > 20.0);
}

// ---- environments -----------------------------------------------------------------------------

TEST(EnvVersionsAndConstraints) {
    using zeromesh::node::ParseVersion;
    using zeromesh::node::Satisfies;
    ASSERT_TRUE(ParseVersion("Blender 4.2.1 (hash abc)")->parts == (std::vector<long>{4, 2, 1}));
    ASSERT_TRUE(ParseVersion("gcc (GCC) 13.2.0")->parts == (std::vector<long>{13, 2, 0}));
    ASSERT_FALSE(ParseVersion("no digits").has_value());
    auto v = *ParseVersion("4.2.1");
    ASSERT_TRUE(*Satisfies(v, ">=4.0"));
    ASSERT_TRUE(*Satisfies(v, ">=4.0,<5"));
    ASSERT_FALSE(*Satisfies(v, ">=4.3"));
    ASSERT_TRUE(*Satisfies(v, "4.2.1"));
    ASSERT_TRUE(*Satisfies(*ParseVersion("4.2"), "==4.2.0"));
    ASSERT_TRUE(*Satisfies(*ParseVersion("4.10"), ">4.9"));
    ASSERT_FALSE(Satisfies(v, ">=").has_value());
}

TEST(EnvIdentityAndHostChecks) {
    v1::Env exact;
    exact.add_programs()->set_name("sh");
    zeromesh::node::StampPrograms(exact);
    ASSERT_FALSE(exact.programs(0).digest().empty());
    ASSERT_FALSE(zeromesh::node::CheckEnv(exact, nullptr).has_value()); // this node is the requester
    v1::Env loose;
    auto* p = loose.add_programs();
    p->set_name("sh");
    p->set_version(">=1");
    ASSERT_NE(zeromesh::node::EnvIdentity(exact), zeromesh::node::EnvIdentity(loose));
    v1::Env missing;
    missing.add_programs()->set_name("definitely-not-a-binary-xyz");
    bool threw = false;
    try {
        zeromesh::node::StampPrograms(missing);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT_TRUE(threw);
    v1::Env tree;
    tree.mutable_tree()->set_hash(std::string(64, 'c'));
    ASSERT_EQ(zeromesh::node::EnvIdentity(tree), "tree:" + std::string(64, 'c'));
}

// ---- placement --------------------------------------------------------------------------------

TEST(PlaceWritesCopiesAndRefusesToEscapeItsRoot) {
    Stores s;
    TempDir dest;
    v1::OutputFile f;
    f.set_path("sub/out.txt");
    *f.mutable_digest() = zeromesh::node::ToProto(s.cas->PutBlob("output"));
    std::string err;
    ASSERT_TRUE(run::Place(*s.cas, *s.merkle, f, dest / "a/b/out.txt", &err));
    ASSERT_EQ(ReadFile(dest / "a/b/out.txt"), "output");
    WriteFile(dest / "a/b/out.txt", "edited");
    ASSERT_EQ(*s.cas->GetBlob(f.digest().hash()), std::string("output")); // the store is untouched
    std::filesystem::path placed;
    ASSERT_TRUE(run::PlaceUnder(*s.cas, *s.merkle, f, dest.path(), &err, &placed));
    ASSERT_EQ(placed, dest / "sub/out.txt");
    f.set_path("../escape.txt");
    ASSERT_FALSE(run::PlaceUnder(*s.cas, *s.merkle, f, dest.path(), &err, &placed));
    std::filesystem::create_symlink("/tmp", dest / "link");
    f.set_path("link/x.txt");
    ASSERT_FALSE(run::PlaceUnder(*s.cas, *s.merkle, f, dest.path(), &err, &placed));
}

// ---- the engine -------------------------------------------------------------------------------

TEST(EngineRunsItemsPlacesOutputsAndWritesTheReport) {
    Engine e;
    TempDir ws, out;
    WriteFile(ws / "in.txt", "x");
    auto spec = e.Spec(ws.path());
    spec.set_report_path((out / "report.json").string());
    for (int i = 0; i < 6; ++i) {
        auto it = MakeItem("item" + std::to_string(i), {"tool", std::to_string(i)});
        auto* o = it.add_outputs();
        o->set_path("r.txt");
        o->set_dest((out / ("r" + std::to_string(i) + ".txt")).string());
        *spec.add_items() = it;
    }
    const std::string id = e.engine->Submit(spec);
    auto rep = e.Wait(id);
    ASSERT_EQ(rep.run().succeeded(), 6);
    ASSERT_EQ(ReadFile(out / "r3.txt"), "out:item3");
    ASSERT_EQ(Result(rep, "item3").outputs(0).placed(), (out / "r3.txt").string());
    ASSERT_EQ(Result(rep, "item3").attempts(0).hostname(), std::string("host-donor-1"));
    ASSERT_TRUE(std::filesystem::exists(out / "report.json"));
    ASSERT_TRUE(std::filesystem::exists(out / "report.trace.json"));
    ASSERT_TRUE(ReadFile(out / "report.json").find("\"succeeded\": 6") != std::string::npos);
    ASSERT_TRUE(std::filesystem::exists(e.s.dir / "runs" / (id + ".spec")));
    ASSERT_EQ(rep.timeline_size(), 6);
}

TEST(EngineServesUnchangedItemsFromTheCacheAndExplainsMisses) {
    Engine e;
    TempDir ws, out;
    WriteFile(ws / "a/in.txt", "a");
    WriteFile(ws / "b/in.txt", "b");
    auto make = [&] {
        auto spec = e.Spec(ws.path());
        for (const char* name : {"a", "b"}) {
            auto it = MakeItem(name, {"tool"});
            it.set_cwd(name);
            it.add_inputs(name);
            auto* o = it.add_outputs();
            o->set_path("r.txt");
            o->set_dest((out / (std::string(name) + ".txt")).string());
            *spec.add_items() = it;
        }
        return spec;
    };
    auto first = e.Wait(e.engine->Submit(make()));
    ASSERT_EQ(first.run().cached(), 0);
    ASSERT_EQ(Result(first, "a").cache().reason(), std::string("no earlier run of this command"));
    std::filesystem::remove(out / "a.txt");

    WriteFile(ws / "b/in.txt", "b changed");
    auto second = e.Wait(e.engine->Submit(make()));
    ASSERT_EQ(second.run().cached(), 1);
    ASSERT_TRUE(Result(second, "a").cache().hit());
    ASSERT_EQ(ReadFile(out / "a.txt"), "out:a"); // placed again from the cache
    ASSERT_FALSE(Result(second, "b").cache().hit());
    ASSERT_EQ(Result(second, "b").cache().changed(0), std::string("b/in.txt"));
    int executed = 0;
    for (const auto& b : e.exec->batches) executed += b.items_size();
    ASSERT_EQ(executed, 3); // a and b, then only b

    // Off by policy: everything runs.
    auto off = make();
    off.mutable_policy()->set_cache(false);
    auto third = e.Wait(e.engine->Submit(off));
    ASSERT_EQ(third.run().cached(), 0);
}

TEST(EngineDepfileItemsHitUntilAFileTheyReadChanges) {
    Engine e;
    TempDir ws;
    WriteFile(ws / "src/a.c", "int a;");
    WriteFile(ws / "include/a.h", "int x;");
    WriteFile(ws / "README", "r");
    e.exec->script["a.o"] = {ScriptedExecutor::Answer{.footprint = {"src/a.c", "include/a.h"}}};
    auto make = [&] {
        auto spec = e.Spec(ws.path());
        auto it = MakeItem("a.o", {"cc", "-c", "a.c"});
        it.set_cwd("src");
        it.set_depfile("out/a.d");
        it.add_search_dirs("../include");
        it.add_outputs()->set_path("out/a.o");
        *spec.add_items() = it;
        return spec;
    };
    e.Wait(e.engine->Submit(make()));
    WriteFile(ws / "README", "unrelated change");
    auto hit = e.Wait(e.engine->Submit(make()));
    Expect(Result(hit, "a.o").cache().hit(), Result(hit, "a.o").cache().reason());
    WriteFile(ws / "include/a.h", "int x2;");
    auto miss = e.Wait(e.engine->Submit(make()));
    ASSERT_FALSE(Result(miss, "a.o").cache().hit());
    ASSERT_EQ(Result(miss, "a.o").cache().changed(0), std::string("include/a.h"));
}

TEST(EngineRetriesInfraFailuresElsewhereAndFallsBackToThisNode) {
    Engine e;
    TempDir ws;
    using A = ScriptedExecutor::Answer;
    e.exec->script["flaky"] = {A{.donor_lost = true}, A{}};
    e.exec->script["nowhere"] = {A{.donor_lost = true, .batch_failure = v1::FAILURE_ENV_UNAVAILABLE}, A{}};
    auto spec = e.Spec(ws.path());
    spec.mutable_policy()->set_keep_going(true);
    *spec.add_items() = MakeItem("flaky", {"t"});
    *spec.add_items() = MakeItem("nowhere", {"t"});
    auto rep = e.Wait(e.engine->Submit(spec));
    ASSERT_EQ(rep.run().succeeded(), 2);
    // The retry of `flaky` never went back to the donor that lost it.
    bool excluded = false;
    for (size_t i = 0; i < e.exec->batches.size(); ++i) {
        for (const auto& it : e.exec->batches[i].items()) {
            if (it.id() == "flaky" && e.exec->excludes[i].contains("donor-1")) excluded = true;
        }
    }
    ASSERT_TRUE(excluded);
    // `nowhere` could run on no donor: it got a try on this node.
    bool local = false;
    for (const auto& b : e.exec->batches) {
        if (b.items(0).id() == "nowhere" && b.where() == v1::LOCAL_ONLY) local = true;
    }
    ASSERT_TRUE(local);
}

TEST(EngineFailureKindsAndRetriesFollowPolicy) {
    Engine e;
    TempDir ws;
    using A = ScriptedExecutor::Answer;
    e.exec->script["exit"] = {A{.item_failure = v1::FAILURE_EXIT, .exit_code = 3}};
    e.exec->script["flaky-exit"] = {A{.item_failure = v1::FAILURE_EXIT, .exit_code = 1}, A{}};
    // Killed when the node's budget ran out, holding 400 MiB.
    e.exec->script["oom"] = {A{.item_failure = v1::FAILURE_OOM, .peak_mem = 400ll << 20}, A{}};
    e.exec->script["hung"] = {A{.item_failure = v1::FAILURE_HUNG}};
    auto spec = e.Spec(ws.path());
    spec.mutable_policy()->set_keep_going(true);
    spec.mutable_policy()->set_exit_retries(0);
    for (const char* id : {"exit", "oom", "hung"}) *spec.add_items() = MakeItem(id, {"t", id});
    auto rep = e.Wait(e.engine->Submit(spec));
    ASSERT_TRUE(Result(rep, "exit").failure().kind() == v1::FAILURE_EXIT);
    ASSERT_EQ(Result(rep, "exit").failure().exit_code(), 3);
    ASSERT_TRUE(Result(rep, "hung").failure().kind() == v1::FAILURE_HUNG);
    // Killed beside other work: retried with a node's memory to itself, known to need at least what it
    // held (reserving that plus the margin), with no hard limit since none was declared; it succeeded.
    ASSERT_TRUE(Result(rep, "oom").state() == v1::ITEM_SUCCEEDED);
    const v1::BatchSpec* retry = nullptr;
    for (const auto& b : e.exec->batches) {
        if (b.items(0).id() == "oom" && b.alone()) retry = &b;
    }
    ASSERT_TRUE(retry != nullptr);
    ASSERT_EQ(retry->mem_need(), 400ll << 20);
    ASSERT_EQ(retry->resources().mem_bytes(), 500ll << 20);
    ASSERT_EQ(retry->mem_limit(), 0);

    // Killed again while alone on a node whose work may use 512 MiB: it needs more than that node
    // allows, which the next attempt says, so only a node allowing more can run it.
    auto spec4 = e.Spec(ws.path());
    spec4.mutable_stages(0)->set_kind("big"); // its own kind: nothing learned from the items above
    e.exec->script["big"] = {A{.item_failure = v1::FAILURE_OOM, .peak_mem = 300ll << 20, .budget = 512ll << 20},
                             A{.item_failure = v1::FAILURE_OOM, .peak_mem = 511ll << 20, .budget = 512ll << 20}, A{}};
    *spec4.add_items() = MakeItem("big", {"t", "big"});
    auto rep4 = e.Wait(e.engine->Submit(spec4));
    ASSERT_TRUE(Result(rep4, "big").state() == v1::ITEM_SUCCEEDED);
    std::vector<v1::BatchSpec> big;
    for (const auto& b : e.exec->batches) {
        if (b.items(0).id() == "big") big.push_back(b);
    }
    ASSERT_EQ(big.size(), static_cast<size_t>(3));
    ASSERT_FALSE(big[0].alone());
    ASSERT_TRUE(big[1].alone());
    ASSERT_EQ(big[1].mem_need(), 300ll << 20);
    ASSERT_EQ(big[2].mem_need(), (512ll << 20) + 1);

    // Past a declared limit of 256 MiB: the limit (and reservation) go up by half.
    auto spec3 = e.Spec(ws.path());
    spec3.mutable_stages(0)->mutable_resources()->set_mem_bytes(256ll << 20);
    e.exec->script["oom-limit"] = {A{.item_failure = v1::FAILURE_OOM, .peak_mem = 256ll << 20, .oom_at_limit = true}, A{}};
    *spec3.add_items() = MakeItem("oom-limit", {"t", "oom-limit"});
    auto rep3 = e.Wait(e.engine->Submit(spec3));
    ASSERT_TRUE(Result(rep3, "oom-limit").state() == v1::ITEM_SUCCEEDED);
    int64_t limit_after = 0;
    for (const auto& b : e.exec->batches) {
        if (b.items(0).id() == "oom-limit") limit_after = std::max(limit_after, b.mem_limit());
    }
    ASSERT_EQ(limit_after, 384ll << 20);

    auto spec2 = e.Spec(ws.path());
    spec2.mutable_policy()->set_exit_retries(1);
    *spec2.add_items() = MakeItem("flaky-exit", {"t"});
    auto rep2 = e.Wait(e.engine->Submit(spec2));
    ASSERT_TRUE(Result(rep2, "flaky-exit").state() == v1::ITEM_SUCCEEDED);
    ASSERT_EQ(Result(rep2, "flaky-exit").attempts_size(), 2);
}

TEST(EngineStopsAtTheFirstFailureUnlessKeepGoing) {
    Engine e;
    TempDir ws;
    e.slots = 1;
    using A = ScriptedExecutor::Answer;
    e.exec->script["bad"] = {A{.item_failure = v1::FAILURE_EXIT, .exit_code = 1, .wall_ms = 10}};
    auto spec = e.Spec(ws.path());
    spec.mutable_policy()->set_batch(1);
    *spec.add_items() = MakeItem("bad", {"t"});
    for (int i = 0; i < 20; ++i) *spec.add_items() = MakeItem("later" + std::to_string(i), {"t", std::to_string(i)});
    auto rep = e.Wait(e.engine->Submit(spec));
    ASSERT_EQ(rep.run().failed(), 1);
    ASSERT_TRUE(rep.run().cancelled() > 0);
    ASSERT_EQ(rep.run().ended_by(), std::string("failure"));
}

TEST(EngineWaitsForDependenciesAndSeesTheirOutputs) {
    Engine e;
    TempDir ws;
    auto spec = e.Spec(ws.path());
    auto first = MakeItem("gen", {"gen"});
    first.add_outputs()->set_path("gen/header.h");
    auto second = MakeItem("use", {"use"});
    second.add_after("gen");
    *spec.add_items() = second; // order in the spec does not matter
    *spec.add_items() = first;
    auto rep = e.Wait(e.engine->Submit(spec));
    ASSERT_EQ(rep.run().succeeded(), 2);
    // `use` ran on an input tree with gen's output in it.
    for (const auto& b : e.exec->batches) {
        if (b.items(0).id() != "use") continue;
        auto entry = e.s.merkle->Lookup(zeromesh::node::FromProto(b.input_root()), "gen/header.h");
        ASSERT_TRUE(entry.kind == store::TreeEntry::Kind::File);
    }
    // A cycle is refused at submit time; a failed dependency fails its dependents.
    auto cyc = e.Spec(ws.path());
    auto x = MakeItem("x", {"t"});
    x.add_after("y");
    auto y = MakeItem("y", {"t"});
    y.add_after("x");
    *cyc.add_items() = x;
    *cyc.add_items() = y;
    bool refused = false;
    try {
        e.engine->Submit(cyc);
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    ASSERT_TRUE(refused);
    e.exec->script["base"] = {ScriptedExecutor::Answer{.item_failure = v1::FAILURE_EXIT, .exit_code = 2}};
    auto dep = e.Spec(ws.path());
    dep.mutable_policy()->set_keep_going(true);
    *dep.add_items() = MakeItem("base", {"t"});
    auto top = MakeItem("top", {"t"});
    top.add_after("base");
    *dep.add_items() = top;
    auto rep2 = e.Wait(e.engine->Submit(dep));
    ASSERT_TRUE(Result(rep2, "top").failure().kind() == v1::FAILURE_DEPENDENCY);
}

TEST(EngineCombinesConsecutiveItemsAndBatchesLongestFirst) {
    Engine e;
    TempDir ws;
    e.slots = 2;
    auto spec = e.Spec(ws.path());
    spec.mutable_stages(0)->mutable_combine()->add_argv("render");
    spec.mutable_stages(0)->mutable_combine()->add_argv("{first}");
    spec.mutable_stages(0)->mutable_combine()->add_argv("{last}");
    spec.mutable_policy()->set_batch(4);
    for (int f = 1; f <= 10; ++f) {
        if (f == 6) continue; // a gap: 5 and 7 are not consecutive
        auto it = MakeItem("frame" + std::to_string(f), {});
        it.set_seq(f);
        *spec.add_items() = it;
    }
    auto rep = e.Wait(e.engine->Submit(spec));
    ASSERT_EQ(rep.run().succeeded(), 9);
    std::vector<std::pair<int64_t, int64_t>> ranges;
    for (const auto& b : e.exec->batches) {
        ASSERT_EQ(b.combine().argv_size(), 3);
        ranges.emplace_back(b.items(0).seq(), b.items(b.items_size() - 1).seq());
        for (int i = 1; i < b.items_size(); ++i) ASSERT_EQ(b.items(i).seq(), b.items(i - 1).seq() + 1);
    }
    std::sort(ranges.begin(), ranges.end());
    ASSERT_TRUE(ranges == (std::vector<std::pair<int64_t, int64_t>>{{1, 4}, {5, 5}, {7, 10}}));

    // Longest first: with learned costs, the slowest item leads the first batch.
    Engine e2;
    e2.resources->Record("k", 1, 50000, 0);
    e2.resources->Record("k", 1 << 20, 900000, 0);
    auto spec2 = e2.Spec(ws.path());
    spec2.mutable_stages(0)->set_kind("k");
    spec2.mutable_policy()->set_batch(1);
    auto small = MakeItem("small", {"t", "s"});
    small.set_cost(1);
    auto big = MakeItem("big", {"t", "b"});
    big.set_cost(1 << 20);
    *spec2.add_items() = small;
    *spec2.add_items() = big;
    e2.Wait(e2.engine->Submit(spec2));
    ASSERT_EQ(e2.exec->batches.front().items(0).id(), std::string("big"));
}

TEST(EngineFormsABatchOnlyForAFreeSlot) {
    // Never ahead of a slot: an item bound to a batch that waits for one could not run on the next
    // slot that frees.
    Engine e;
    TempDir ws;
    e.slots = 2;
    auto spec = e.Spec(ws.path());
    spec.mutable_policy()->set_batch(1);
    for (int i = 0; i < 10; ++i) *spec.add_items() = MakeItem("i" + std::to_string(i), {"t", std::to_string(i)});
    auto rep = e.Wait(e.engine->Submit(spec));
    ASSERT_EQ(rep.run().succeeded(), 10);
    ASSERT_EQ(e.exec->peak_active, 2);
}

TEST(EngineSpreadsTheLongItemsOfAFixedBatchSize) {
    // --batch 4 over 4 long and 12 short items, 4 slots: each batch gets one long item, not one
    // batch all four (the run would last as long as that batch).
    Engine e;
    TempDir ws;
    e.slots = 4;
    e.resources->Record("k", 1, 50000, 0);
    e.resources->Record("k", 1 << 20, 900000, 0);
    auto spec = e.Spec(ws.path());
    spec.mutable_stages(0)->set_kind("k");
    spec.mutable_policy()->set_batch(4);
    for (int i = 0; i < 16; ++i) {
        auto it = MakeItem("i" + std::to_string(i), {"t", std::to_string(i)});
        it.set_cost(i < 4 ? (1 << 20) : 1); // i0..i3 are long
        *spec.add_items() = it;
    }
    e.Wait(e.engine->Submit(spec));
    ASSERT_EQ(e.exec->batches.size(), static_cast<size_t>(4));
    for (const auto& b : e.exec->batches) {
        ASSERT_EQ(b.items_size(), 4);
        int long_items = 0;
        for (const auto& it : b.items()) long_items += std::stoi(it.id().substr(1)) < 4 ? 1 : 0;
        ASSERT_EQ(long_items, 1);
    }
}

TEST(EngineSessionsTakeItemsUntilClosedAndResumeUsesTheCache) {
    Engine e;
    TempDir ws;
    auto spec = e.Spec(ws.path());
    spec.set_open(true);
    *spec.add_items() = MakeItem("one", {"t", "1"});
    const std::string id = e.engine->Submit(spec);
    std::this_thread::sleep_for(300ms);
    ASSERT_FALSE(e.engine->Status(id)->done()); // open: more may come
    e.engine->AddItems(id, {MakeItem("two", {"t", "2"})});
    e.engine->Close(id);
    auto rep = e.Wait(id);
    ASSERT_EQ(rep.run().succeeded(), 2);
    const size_t ran = e.exec->batches.size();
    const std::string again = e.engine->Resume(id);
    auto rep2 = e.Wait(again);
    ASSERT_EQ(rep2.run().cached(), 2);
    ASSERT_EQ(e.exec->batches.size(), ran); // nothing ran
    auto runs = e.engine->List();
    ASSERT_TRUE(runs.size() >= 2);
}

TEST(EngineCancelsASessionNoClientWatches) {
    Engine e(nullptr, 400ms);
    TempDir ws;
    auto spec = e.Spec(ws.path());
    spec.set_open(true);
    *spec.add_items() = MakeItem("one", {"t", "1"});

    // Watched, it stays open past the grace and takes items.
    const std::string id = e.engine->Submit(spec);
    auto events = e.engine->Watch(id, true);
    std::this_thread::sleep_for(1s);
    e.engine->AddItems(id, {MakeItem("two", {"t", "2"})});
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (e.engine->Status(id)->succeeded() < 2) {
        ASSERT_TRUE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_FALSE(e.engine->Status(id)->done());
    // Its client gone (its watch ends with its process), the run is cancelled after the grace.
    events->Close();
    auto rep = e.Wait(id);
    ASSERT_EQ(rep.run().ended_by(), std::string("disconnect"));
    ASSERT_EQ(rep.run().succeeded(), 2);
    bool refused = false;
    try {
        e.engine->AddItems(id, {MakeItem("three", {"t", "3"})});
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    ASSERT_TRUE(refused);

    // Never watched: the same.
    ASSERT_EQ(e.Wait(e.engine->Submit(spec)).run().ended_by(), std::string("disconnect"));
    // Closed, it needs no watcher: it ends once its items are done.
    const std::string closed = e.engine->Submit(spec);
    e.engine->Close(closed);
    auto done = e.Wait(closed);
    ASSERT_EQ(done.run().ended_by(), std::string());
    ASSERT_EQ(done.run().succeeded(), 1);
}

TEST(EngineCancelEndsTheRunAndKeepsFinishedItems) {
    Engine e;
    TempDir ws;
    e.slots = 1;
    using A = ScriptedExecutor::Answer;
    for (int i = 0; i < 30; ++i) e.exec->script["i" + std::to_string(i)] = {A{.wall_ms = 10}};
    auto spec = e.Spec(ws.path());
    spec.mutable_policy()->set_batch(1);
    for (int i = 0; i < 30; ++i) *spec.add_items() = MakeItem("i" + std::to_string(i), {"t", std::to_string(i)});
    const std::string id = e.engine->Submit(spec);
    std::this_thread::sleep_for(150ms);
    e.engine->Cancel(id);
    auto rep = e.Wait(id);
    ASSERT_EQ(rep.run().ended_by(), std::string("cancel"));
    ASSERT_TRUE(rep.run().cancelled() > 0);
    ASSERT_EQ(rep.run().succeeded() + rep.run().cancelled(), 30);
}

TEST(EngineRejectsMalformedRuns) {
    Engine e;
    TempDir ws;
    auto refused = [&](v1::RunSpec spec) {
        try {
            e.engine->Submit(std::move(spec));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    auto dup = e.Spec(ws.path());
    *dup.add_items() = MakeItem("x", {"t"});
    *dup.add_items() = MakeItem("x", {"t"});
    ASSERT_TRUE(refused(dup));
    auto stage = e.Spec(ws.path());
    auto it = MakeItem("x", {"t"});
    it.set_stage("nope");
    *stage.add_items() = it;
    ASSERT_TRUE(refused(stage));
    auto escape = e.Spec(ws.path());
    auto it2 = MakeItem("x", {"t"});
    it2.add_outputs()->set_path("../../etc/passwd");
    *escape.add_items() = it2;
    ASSERT_TRUE(refused(escape));
    auto rel = e.Spec(ws.path());
    auto it3 = MakeItem("x", {"t"});
    auto* o = it3.add_outputs();
    o->set_path("a");
    o->set_dest("relative/dest");
    *rel.add_items() = it3;
    ASSERT_TRUE(refused(rel));
    auto noprog = e.Spec(ws.path());
    noprog.mutable_stages(0)->mutable_env()->add_programs()->set_name("definitely-not-a-binary-xyz");
    *noprog.add_items() = MakeItem("x", {"t"});
    ASSERT_TRUE(refused(noprog));
    auto spaces = e.Spec(ws.path());
    *spaces.add_items() = MakeItem("has space", {"t"});
    ASSERT_TRUE(refused(spaces));
}

// A team cache with one other node that has a result.
class OneNodeTeam final : public run::Team {
public:
    std::shared_ptr<run::ItemCache> remote;
    std::shared_ptr<store::CasStore> remote_cas;
    std::shared_ptr<store::CasStore> local_cas;
    int lookups{0};
    std::vector<std::pair<std::string, v1::CacheLookupReply>> Lookup(const v1::CacheLookup& req) override {
        ++lookups;
        return {{"peer-1", remote->Lookup(req)}};
    }
    std::optional<v1::CacheLookupReply> LookupAt(const std::string&, const v1::CacheLookup& req) override {
        return remote->Lookup(req);
    }
    bool Pull(const std::string&, const std::vector<v1::Digest>& blobs, const std::vector<v1::Digest>&) override {
        for (const auto& b : blobs) {
            auto data = remote_cas->GetBlob(b.hash());
            if (!data) return false;
            local_cas->PutBlob(*data);
        }
        return true;
    }
    int Forget(const v1::CacheForget&) override { return 1; }
};

TEST(EngineFindsResultsOnTheTeam) {
    // Node 1 runs the item; node 2, with its own empty cache, gets it from node 1.
    auto team = std::make_shared<OneNodeTeam>();
    Engine first;
    TempDir ws, out;
    WriteFile(ws / "in.txt", "same inputs on both nodes");
    auto make = [&](Engine& e) {
        auto spec = e.Spec(ws.path());
        auto it = MakeItem("item", {"tool"});
        auto* o = it.add_outputs();
        o->set_path("r.txt");
        o->set_dest((out / "r.txt").string());
        *spec.add_items() = it;
        return spec;
    };
    first.Wait(first.engine->Submit(make(first)));
    team->remote = first.cache;
    team->remote_cas = first.s.cas;
    Engine second(team);
    team->local_cas = second.s.cas;
    std::filesystem::remove(out / "r.txt");
    auto rep = second.Wait(second.engine->Submit(make(second)));
    Expect(Result(rep, "item").cache().hit(), Result(rep, "item").cache().reason());
    ASSERT_EQ(Result(rep, "item").cache().source(), std::string("peer-1"));
    ASSERT_EQ(ReadFile(out / "r.txt"), "out:item");
    ASSERT_TRUE(second.exec->batches.empty());
    ASSERT_TRUE(second.cache->Get(Result(rep, "item").cache().key()).has_value()); // kept here too
}

// ---- end to end: engine + real placer + donors ------------------------------------------------

TEST(EngineEndToEndRetriesAnItemWhenItsDonorIsLost) {
    auto key = zeromesh::net::GenerateTeamKey();
    zeromesh::testing::TestNodeFull a("node-a", key), b("node-b", key), c("node-c", key);
    a.Start();
    b.Start();
    c.Start();
    a.Admit(b);
    a.Admit(c);
    a.peers->Upsert(b.AsPeer());
    a.peers->Upsert(c.AsPeer());
    zeromesh::testing::PlacerRequester req(a);
    run::EngineDeps d;
    d.cas = a.cas;
    d.merkle = a.merkle;
    d.cache = std::make_shared<run::ItemCache>(a.cas, a.merkle);
    d.executor = req.placer;
    d.resources = std::make_shared<run::ResourceModel>();
    d.slots = [&](const v1::Resources& r, v1::Where w) { return req.placer->SlotsFor(r, w); };
    d.runs_dir = a.dir / "runs";
    d.node_id = "node-a";
    d.arch = zeromesh::testing::kTestArch;
    run::RunEngine engine(std::move(d));

    TempDir ws, out;
    WriteFile(ws / "in.txt", "payload");
    v1::RunSpec spec;
    spec.set_name("e2e");
    *spec.mutable_workspace() = zeromesh::node::ToProto(a.merkle->BuildTree(ws.path()));
    auto* st = spec.add_stages();
    st->set_where(v1::REMOTE_ONLY);
    auto* it = spec.add_items();
    it->set_id("slow");
    it->add_argv("sh");
    it->add_argv("-c");
    it->add_argv("sleep 3; printf %s \"$ZM_NODE_ID\" > who");
    auto* o = it->add_outputs();
    o->set_path("who");
    o->set_dest((out / "who").string());
    const std::string id = engine.Submit(spec);

    // Wait until it runs somewhere, then stop that donor.
    std::string where;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (where.empty() && std::chrono::steady_clock::now() < deadline) {
        if (req.placer->InFlightOn("node-b") > 0) where = "node-b";
        if (req.placer->InFlightOn("node-c") > 0) where = "node-c";
        std::this_thread::sleep_for(50ms);
    }
    ASSERT_FALSE(where.empty());
    std::this_thread::sleep_for(500ms);
    (where == "node-b" ? b : c).Stop();

    v1::Report rep;
    const auto end = std::chrono::steady_clock::now() + 40s;
    while (std::chrono::steady_clock::now() < end) {
        if (engine.Status(id)->done()) break;
        std::this_thread::sleep_for(50ms);
    }
    rep = *engine.Report(id);
    const auto& r = rep.items(0);
    Expect(r.state() == v1::ITEM_SUCCEEDED, r.failure().message());
    const std::string survivor = where == "node-b" ? "node-c" : "node-b";
    ASSERT_EQ(ReadFile(out / "who"), survivor);
    ASSERT_EQ(r.attempts_size(), 1); // the lost attempt reported no outcome; one retry ran it
}
