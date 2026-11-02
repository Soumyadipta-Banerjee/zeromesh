#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "exec/batch_runner.hpp"
#include "exec/cgroup.hpp"
#include "exec/depfile.hpp"
#include "exec/input_cache.hpp"
#include "exec/tree_watch.hpp"
#include "plugins/env-host/env_host.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using zeromesh::exec::BatchCallbacks;
using zeromesh::exec::BatchRunner;
using zeromesh::exec::BatchRunSpec;
using zeromesh::exec::ItemFailure;
using zeromesh::exec::ItemRunResult;
using zeromesh::exec::ItemSpec;
using zeromesh::testing::ReadFile;
using zeromesh::testing::TempDir;
using zeromesh::testing::WriteFile;

struct Fixture {
    TempDir dir;
    std::shared_ptr<zeromesh::store::CasStore> cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    std::shared_ptr<zeromesh::store::MerkleBuilder> merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    BatchRunner runner{cas, merkle, dir / "scratch"};

    std::mutex mu;
    std::map<std::string, ItemRunResult> results;
    std::vector<std::string> order; // item ids in the order on_item reported them
    std::vector<std::pair<std::string, zeromesh::store::Digest>> checkpoints;

    BatchRunSpec Spec() {
        BatchRunSpec s;
        s.sandbox_env = *zeromesh::envhost::Make()->Prepare({}).value;
        return s;
    }

    static ItemSpec Sh(const std::string& id, const std::string& script) {
        ItemSpec it;
        it.id = id;
        it.argv = {"sh", "-c", script};
        return it;
    }

    zeromesh::exec::BatchRunResult Run(const BatchRunSpec& spec, std::stop_token stop = {}) {
        BatchCallbacks cb;
        cb.on_item = [this](ItemRunResult r) {
            std::lock_guard lock(mu);
            order.push_back(r.id);
            results[r.id] = std::move(r);
        };
        cb.on_checkpoint = [this](const std::string& item, const zeromesh::store::Digest& d) {
            std::lock_guard lock(mu);
            checkpoints.emplace_back(item, d);
        };
        return runner.Run(spec, cb, stop);
    }

    std::string Blob(const zeromesh::store::Digest& d) { return cas->GetBlob(d.hash).value_or("<missing>"); }

    std::string Output(const std::string& item, const std::string& path) {
        for (const auto& o : results.at(item).outputs) {
            if (o.path == path) return Blob(o.digest);
        }
        return "<absent>";
    }

    bool Usable() {
        if (!runner.unavailable().empty()) {
            std::cout << "[SKIP] " << runner.unavailable() << "\n";
            return false;
        }
        return true;
    }
};

} // namespace

TEST(DepfileParsesGccOutput) {
    auto deps = zeromesh::exec::ParseDepfile("out/a.o: src/a.c include/a.h \\\n /usr/include/stdio.h \\\n"
                                             "  my\\ file.h $$weird.h\n\ninclude/a.h:\n/usr/include/stdio.h:\n");
    ASSERT_EQ(deps.size(), 5u);
    ASSERT_EQ(deps[0], "src/a.c");
    ASSERT_EQ(deps[1], "include/a.h");
    ASSERT_EQ(deps[2], "/usr/include/stdio.h");
    ASSERT_EQ(deps[3], "my file.h");
    ASSERT_EQ(deps[4], "$weird.h");
    ASSERT_TRUE(zeromesh::exec::ParseDepfile("x.o x.d: x.c\r\n").size() == 1u);
}

TEST(TreeWatchReportsClosedAndRenamedFilesInNewDirectories) {
    TempDir d;
    zeromesh::exec::TreeWatch w(d.path());
    ASSERT_TRUE(w.ok());
    fs::create_directories(d / "out");
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(w.Poll(50ms).empty()); // creating a directory finishes nothing
    WriteFile(d / "out/a.png", "a");
    WriteFile(d / "out/b.tmp", "b");
    fs::rename(d / "out/b.tmp", d / "out/b.png");
    std::vector<std::string> seen;
    for (int i = 0; i < 10 && seen.size() < 3; ++i) {
        for (auto& p : w.Poll(50ms)) seen.push_back(p);
    }
    ASSERT_TRUE(std::find(seen.begin(), seen.end(), "out/a.png") != seen.end());
    ASSERT_TRUE(std::find(seen.begin(), seen.end(), "out/b.png") != seen.end());
}

TEST(InputCacheMaterializesOnceAndPrunesIdleEntries) {
    TempDir d;
    auto cas = std::make_shared<zeromesh::store::CasStore>(d / "cas");
    auto merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    WriteFile(d / "src/x.txt", "x");
    auto root = merkle->BuildTree(d / "src");
    zeromesh::exec::InputCache cache(d / "inputs", merkle);
    auto a = cache.Acquire(root);
    auto b = cache.Acquire(root);
    ASSERT_TRUE(a && b);
    ASSERT_EQ(a->path(), b->path());
    ASSERT_EQ(ReadFile(a->path() / "x.txt"), "x");
    ASSERT_EQ(cache.Entries(), 1u);
    cache.Prune(0s);
    ASSERT_TRUE(fs::exists(a->path())); // still leased
    a.reset();
    b.reset();
    cache.Prune(0s);
    ASSERT_EQ(cache.Entries(), 0u);
    ASSERT_FALSE(cache.Acquire(zeromesh::store::Digest{std::string(64, 'a'), 1})); // not in the store
}

TEST(BatchRunsEachItemInItsOwnSandboxWithItsOwnResult) {
    Fixture f;
    if (!f.Usable()) return;
    TempDir in;
    WriteFile(in / "src/in.txt", "input");
    auto spec = f.Spec();
    spec.input_root = f.merkle->BuildTree(in.path());
    auto a = Fixture::Sh("a", "echo $ZM_ITEM_ID; cat in.txt > out.txt; echo leaked > shared.txt");
    a.cwd = "src";
    a.outputs = {"out.txt"};
    auto b = Fixture::Sh("b", "test ! -e shared.txt && echo clean > out.txt; exit 3");
    b.cwd = "src";
    b.outputs = {"out.txt"};
    spec.items = {a, b};
    auto r = f.Run(spec);
    ASSERT_EQ(r.error, std::string());
    ASSERT_FALSE(r.cancelled);
    ASSERT_EQ(f.order.size(), 2u);
    ASSERT_TRUE(f.results["a"].failure == ItemFailure::None);
    ASSERT_EQ(f.results["a"].exit_code, 0);
    ASSERT_EQ(f.Output("a", "out.txt"), "input");
    ASSERT_EQ(f.Blob(f.results["a"].stdout_digest), "a\n");
    ASSERT_EQ(f.results["a"].stdout_tail, "a\n");
    // b does not see what a wrote, and its exit code is its own.
    ASSERT_TRUE(f.results["b"].failure == ItemFailure::Exit);
    ASSERT_EQ(f.results["b"].exit_code, 3);
}

TEST(BatchNeverWritesThroughToTheStore) {
    Fixture f;
    if (!f.Usable()) return;
    TempDir in;
    WriteFile(in / "data.txt", "original");
    auto spec = f.Spec();
    spec.input_root = f.merkle->BuildTree(in.path());
    auto it = Fixture::Sh("w", "echo modified >> data.txt; rm -f nothing; echo new > fresh.txt");
    it.capture_changed = true;
    spec.items = {it};
    f.Run(spec);
    ASSERT_TRUE(f.results["w"].failure == ItemFailure::None);
    const auto blob = f.cas->GetBlobPath(zeromesh::store::CasStore::ComputeDigest("original").hash);
    ASSERT_EQ(ReadFile(blob), "original");
    ASSERT_EQ(f.Output("w", "data.txt"), "originalmodified\n");
    ASSERT_EQ(f.Output("w", "fresh.txt"), "new\n");
    ASSERT_EQ(f.results["w"].outputs.size(), 2u); // unchanged inputs are not outputs
}

TEST(BatchClassifiesMissingOutputNotFoundAndEscapes) {
    Fixture f;
    if (!f.Usable()) return;
    auto spec = f.Spec();
    auto missing = Fixture::Sh("missing", "true");
    missing.outputs = {"out/never.txt"};
    missing.optional_outputs = {"maybe.txt"};
    ItemSpec nf;
    nf.id = "nf";
    nf.argv = {"no-such-command-zm"};
    // A task points a directory at the host's /etc and declares a file under it as output.
    auto escape = Fixture::Sh("escape", "rm -rf out; ln -s /etc out");
    escape.outputs = {"out/passwd"};
    spec.items = {missing, nf, escape};
    f.Run(spec);
    ASSERT_TRUE(f.results["missing"].failure == ItemFailure::MissingOutput);
    ASSERT_TRUE(f.results["missing"].message.find("out/never.txt") != std::string::npos);
    ASSERT_TRUE(f.results["nf"].failure == ItemFailure::Exit);
    ASSERT_EQ(f.results["nf"].exit_code, 127);
    ASSERT_EQ(f.results["nf"].message, "command not found");
    ASSERT_TRUE(f.results["escape"].failure == ItemFailure::MissingOutput);
}

TEST(BatchReadsTheFootprintFromTheDepfile) {
    Fixture f;
    if (!f.Usable()) return;
    TempDir in;
    WriteFile(in / "proj/src/a.c", "int a;");
    WriteFile(in / "proj/include/a.h", "int x;");
    auto spec = f.Spec();
    spec.input_root = f.merkle->BuildTree(in.path());
    auto it = Fixture::Sh("cc", "mkdir -p out && printf 'out/a.o: src/a.c ../proj/include/a.h \\\\\\n "
                                "/usr/include/stdio.h /work/proj/src/a.c\\n' "
                                "> out/a.d && echo obj > out/a.o");
    it.cwd = "proj";
    it.outputs = {"out/a.o"};
    it.depfile = "out/a.d";
    spec.items = {it};
    f.Run(spec);
    const auto& r = f.results["cc"];
    ASSERT_TRUE(r.failure == ItemFailure::None);
    ASSERT_EQ(r.footprint.size(), 2u);
    ASSERT_EQ(r.footprint[0], "proj/include/a.h");
    ASSERT_EQ(r.footprint[1], "proj/src/a.c");
}

TEST(BatchCombinedCommitsEachItemWhenItsOutputIsClosed) {
    Fixture f;
    if (!f.Usable()) return;
    auto spec = f.Spec();
    for (int i = 1; i <= 3; ++i) {
        ItemSpec it;
        it.id = "frame_" + std::to_string(i);
        it.seq = i;
        it.outputs = {"out/f" + std::to_string(i) + ".txt"};
        spec.items.push_back(it);
    }
    // One process renders frames {first}..{last}, one second apart.
    spec.combine_argv = {"sh", "-c", "for i in $(seq {first} {last}); do echo frame$i > out/f$i.txt; sleep 1; done"};
    const auto start = std::chrono::steady_clock::now();
    std::map<std::string, double> at;
    BatchCallbacks cb;
    cb.on_item = [&](ItemRunResult r) {
        std::lock_guard lock(f.mu);
        at[r.id] = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        f.results[r.id] = std::move(r);
    };
    auto res = f.runner.Run(spec, cb, {});
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    ASSERT_EQ(res.error, std::string());
    ASSERT_EQ(f.results.size(), 3u);
    for (int i = 1; i <= 3; ++i) {
        const std::string id = "frame_" + std::to_string(i);
        ASSERT_TRUE(f.results[id].failure == ItemFailure::None);
        ASSERT_EQ(f.Output(id, "out/f" + std::to_string(i) + ".txt"), "frame" + std::to_string(i) + "\n");
    }
    // frame_1 was committed while the process was still rendering the others.
    ASSERT_TRUE(at["frame_1"] < total - 1.0);
}

TEST(BatchCombinedCrashLosesOnlyTheItemsInFlight) {
    Fixture f;
    if (!f.Usable()) return;
    auto spec = f.Spec();
    for (int i = 1; i <= 4; ++i) {
        ItemSpec it;
        it.id = "frame_" + std::to_string(i);
        it.seq = i;
        it.outputs = {"out/f" + std::to_string(i) + ".txt"};
        spec.items.push_back(it);
    }
    // Writes frames 1 and 2, then dies at once: no chance to commit while it runs.
    spec.combine_argv = {"sh", "-c", "for i in $(seq {first} {last}); do [ $i -ge 3 ] && exit 9; echo frame$i > out/f$i.txt; done"};
    BatchCallbacks cb;
    cb.on_item = [&](ItemRunResult r) {
        std::lock_guard lock(f.mu);
        f.results[r.id] = std::move(r);
    };
    auto res = f.runner.Run(spec, cb, {});
    ASSERT_EQ(res.error, std::string());
    ASSERT_EQ(f.results.size(), 4u);
    for (int i = 1; i <= 2; ++i) {
        const std::string id = "frame_" + std::to_string(i);
        ASSERT_TRUE(f.results[id].failure == ItemFailure::None);
        ASSERT_EQ(f.Output(id, "out/f" + std::to_string(i) + ".txt"), "frame" + std::to_string(i) + "\n");
    }
    for (const char* id : {"frame_3", "frame_4"}) {
        ASSERT_TRUE(f.results[id].failure == ItemFailure::Exit);
        ASSERT_EQ(f.results[id].exit_code, 9);
    }
}

TEST(BatchCancelSendsSigtermAndKeepsTheCheckpoint) {
    Fixture f;
    if (!f.Usable()) return;
    auto spec = f.Spec();
    spec.term_grace = 5s;
    // On SIGTERM the item saves its progress atomically and exits.
    auto it = Fixture::Sh("long", "trap 'echo 41 > $ZM_CHECKPOINT_DIR/.tmp && mv $ZM_CHECKPOINT_DIR/.tmp "
                                  "$ZM_CHECKPOINT_DIR/progress; exit 0' TERM; while true; do sleep 0.1; done");
    spec.items = {it, Fixture::Sh("never", "true")};
    std::stop_source stop;
    std::thread canceller([&] {
        std::this_thread::sleep_for(800ms);
        stop.request_stop();
    });
    auto r = f.Run(spec, stop.get_token());
    canceller.join();
    ASSERT_TRUE(r.cancelled);
    ASSERT_TRUE(f.results.empty()); // neither item reached an outcome
    ASSERT_FALSE(f.checkpoints.empty());
    ASSERT_EQ(f.checkpoints.back().first, "long");
    auto e = f.merkle->Lookup(f.checkpoints.back().second, "progress");
    ASSERT_EQ(f.Blob(e.digest), "41\n");

    // The next attempt starts from it.
    Fixture g;
    auto spec2 = g.Spec();
    auto resume = Fixture::Sh("long", "cat $ZM_CHECKPOINT_DIR/progress > out.txt");
    resume.outputs = {"out.txt"};
    // Copy the checkpoint tree into g's store.
    TempDir tmp;
    ASSERT_TRUE(f.merkle->MaterializeTree(f.checkpoints.back().second, tmp / "c"));
    resume.checkpoint = g.merkle->BuildTree(tmp / "c");
    spec2.items = {resume};
    g.Run(spec2);
    ASSERT_EQ(g.Output("long", "out.txt"), "41\n");
}

TEST(BatchTimeoutAndHungAreTheirOwnKinds) {
    Fixture f;
    if (!f.Usable()) return;
    auto spec = f.Spec();
    spec.timeout = 1s;
    spec.term_grace = 1s;
    spec.items = {Fixture::Sh("slow", "exec sleep 30")};
    f.Run(spec);
    ASSERT_TRUE(f.results["slow"].failure == ItemFailure::Timeout);

    Fixture g;
    auto spec2 = g.Spec();
    spec2.hung_after = 1s;
    spec2.term_grace = 1s;
    spec2.items = {Fixture::Sh("stuck", "exec sleep 30")};
    const auto start = std::chrono::steady_clock::now();
    g.Run(spec2);
    ASSERT_TRUE(g.results["stuck"].failure == ItemFailure::Hung);
    ASSERT_TRUE(std::chrono::steady_clock::now() - start < 10s);
}

TEST(BatchProcessThatClosesItsPipesStillTimesOut) {
    Fixture f;
    if (!f.Usable()) return;
    auto spec = f.Spec();
    spec.timeout = 1s;
    spec.term_grace = 1s;
    spec.items = {Fixture::Sh("quiet", "exec >/dev/null 2>&1; sleep 30")};
    const auto start = std::chrono::steady_clock::now();
    f.Run(spec);
    ASSERT_TRUE(f.results["quiet"].failure == ItemFailure::Timeout);
    ASSERT_TRUE(std::chrono::steady_clock::now() - start < 10s);
}

TEST(BatchReportsOomAsItsOwnKind) {
    Fixture f;
    if (!f.Usable()) return;
    if (!zeromesh::exec::cgroup::Probe().memory) {
        std::cout << "[SKIP] no delegated memory controller\n";
        return;
    }
    auto spec = f.Spec();
    spec.mem_bytes = 64ull << 20;
    spec.items = {Fixture::Sh("hog", "python3 -c 'b = bytearray(512 * 1024 * 1024); print(len(b))'")};
    f.Run(spec);
    ASSERT_TRUE(f.results["hog"].failure == ItemFailure::Oom);
}

TEST(BatchMeasuresTheItemsMemoryNotTheSandboxes) {
    Fixture f;
    if (!f.Usable()) return;
    auto spec = f.Spec();
    // 200 MiB touched and held for a second, in a grandchild of the sandbox (sh -> python3).
    spec.items = {Fixture::Sh("big", "python3 -c 'import time; b = bytearray(200 << 20); b[::4096] = b\"x\" * len(b[::4096]); time.sleep(1)'")};
    f.Run(spec);
    ASSERT_TRUE(f.results["big"].failure == ItemFailure::None);
    // wait4 on bwrap saw about 2 MiB; the items' processes held over 200 MiB.
    ASSERT_TRUE(f.results["big"].peak_rss >= (190ll << 20));

    // Its CPU is counted too (wait4 on bwrap reports none of it).
    spec.items = {Fixture::Sh("busy", "python3 -c 'sum(range(60_000_000))'")};
    f.Run(spec);
    if (f.results["busy"].cpu_ms < 150)
        throw std::runtime_error("busy item measured at " + std::to_string(f.results["busy"].cpu_ms) + " ms CPU");

    // A small item is small: nothing outside its own processes is counted.
    spec.items = {Fixture::Sh("small", "sleep 1")};
    f.Run(spec);
    if (f.results["small"].peak_rss >= (64ll << 20))
        throw std::runtime_error("sleep measured at " + std::to_string(f.results["small"].peak_rss >> 20) + " MiB");
}
