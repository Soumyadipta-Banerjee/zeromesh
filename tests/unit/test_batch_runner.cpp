#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "exec/batch_runner.hpp"
#include "exec/cgroup.hpp"
#include "exec/depfile.hpp"
#include "exec/input_cache.hpp"
#include "exec/tree_watch.hpp"
#include "plugins/env-host/env_host.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

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
    // `budget`: the memory all work on this node may use (the process-wide work cgroup's memory.max).
    explicit Fixture(uint64_t budget = 0) : runner{cas, merkle, dir / "scratch", zeromesh::exec::cgroup::SetUpWork(budget)} {}

    TempDir dir;
    std::shared_ptr<zeromesh::store::CasStore> cas = std::make_shared<zeromesh::store::CasStore>(dir / "cas");
    std::shared_ptr<zeromesh::store::MerkleBuilder> merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    BatchRunner runner;

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

// While it lives, this thread's signals are as in zmd: SIGINT and SIGTERM blocked for its
// signalfd, SIGHUP ignored as nohup leaves it.
struct SignalsAsInZmd {
    sigset_t old_mask{};
    struct sigaction old_hup{};

    SignalsAsInZmd() {
        sigset_t sigs;
        sigemptyset(&sigs);
        sigaddset(&sigs, SIGINT);
        sigaddset(&sigs, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &sigs, &old_mask);
        struct sigaction ignore{};
        ignore.sa_handler = SIG_IGN;
        sigaction(SIGHUP, &ignore, &old_hup);
    }
    ~SignalsAsInZmd() {
        sigaction(SIGHUP, &old_hup, nullptr);
        pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
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
    ASSERT_FALSE(cache.AcquireCached(root)); // a lookup never materializes or stats every blob
    auto a = cache.Acquire(root);
    auto b = cache.AcquireCached(root);
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
    ASSERT_FALSE(cache.AcquireCached(root));
    ASSERT_FALSE(cache.Acquire(zeromesh::store::Digest{std::string(64, 'a'), 1})); // not in the store
}

TEST(InputCacheLeaseSurvivesCasGarbageCollection) {
    TempDir d;
    auto cas = std::make_shared<zeromesh::store::CasStore>(d / "cas");
    auto merkle = std::make_shared<zeromesh::store::MerkleBuilder>(cas);
    WriteFile(d / "src/x.txt", "cached input");
    auto root = merkle->BuildTree(d / "src");
    zeromesh::exec::InputCache cache(d / "inputs", merkle);
    auto lease = cache.Acquire(root);
    ASSERT_TRUE(lease);
    ASSERT_TRUE(cas->CollectGarbage(0, {}).blobs_removed > 0);
    ASSERT_FALSE(cas->HasBlob(root.hash));
    auto cached = cache.AcquireCached(root);
    ASSERT_TRUE(cached);
    cache.Prune(0s);
    ASSERT_EQ(ReadFile(cached->path() / "x.txt"), "cached input");
    lease.reset();
    cached.reset();
    cache.Prune(0s);
    ASSERT_FALSE(cache.AcquireCached(root));
    ASSERT_FALSE(cache.Acquire(root)); // manifests were collected, too
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
    // On SIGTERM the item saves its progress atomically and exits. A program with its own handler,
    // as a solver is: a shell clears the signal mask it was started with, a program does not.
    ItemSpec it;
    it.id = "long";
    it.argv = {"python3", "-c",
               "import os, signal, sys, time\n"
               "def save(*_):\n"
               "    d = os.environ['ZM_CHECKPOINT_DIR']\n"
               "    open(d + '/.tmp', 'w').write('41\\n')\n"
               "    os.rename(d + '/.tmp', d + '/progress')\n"
               "    sys.exit(0)\n"
               "signal.signal(signal.SIGTERM, save)\n"
               "while True: time.sleep(0.1)\n"};
    spec.items = {it, Fixture::Sh("never", "true")};
    std::stop_source stop;
    std::thread canceller([&] {
        std::this_thread::sleep_for(800ms);
        stop.request_stop();
    });
    zeromesh::exec::BatchRunResult r;
    {
        const SignalsAsInZmd as_in_zmd; // the item sees the SIGTERM all the same
        r = f.Run(spec, stop.get_token());
    }
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

TEST(BatchItemStartsWithNoSignalBlockedOrIgnored) {
    Fixture f;
    if (!f.Usable()) return;
    auto spec = f.Spec();
    ItemSpec it;
    it.id = "sigs";
    it.argv = {"grep", "-E", "^Sig(Blk|Ign):", "/proc/self/status"}; // no shell between: the item itself
    spec.items = {it};
    {
        const SignalsAsInZmd as_in_zmd;
        f.Run(spec);
    }
    ASSERT_EQ(f.results["sigs"].stdout_tail, "SigBlk:\t0000000000000000\nSigIgn:\t0000000000000000\n");
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
    auto spec = f.Spec();
    spec.mem_bytes = 64ull << 20;
    spec.items = {Fixture::Sh("hog", "python3 -c 'b = bytearray(512 * 1024 * 1024); print(len(b))'")};
    f.Run(spec);
    ASSERT_TRUE(f.results["hog"].failure == ItemFailure::Oom);
    ASSERT_TRUE(f.results["hog"].message.find("the batch's limit is 64 MiB") != std::string::npos);
    ASSERT_TRUE(f.results["hog"].oom_at_limit); // its own limit: it needs more than that
}

TEST(BatchWorkBudgetHoldsWorkThatDeclaresNoLimit) {
    // However wrong an estimate, work cannot take more than the node's budget: an item past it is
    // OOM-killed inside the work cgroup and said to be, though its batch declared no limit.
    struct Restore {
        ~Restore() { zeromesh::exec::cgroup::SetUpWork(0); }
    } restore;
    Fixture f(96ull << 20);
    if (!f.Usable()) return;
    ASSERT_EQ(f.runner.work().budget, 96ull << 20);
    auto spec = f.Spec();
    spec.items = {Fixture::Sh("hog", "python3 -c 'b = bytearray(512 * 1024 * 1024); b[::4096] = b\"x\" * len(b[::4096])'"),
                  Fixture::Sh("small", "echo fits")};
    f.Run(spec);
    ASSERT_TRUE(f.results["hog"].failure == ItemFailure::Oom);
    ASSERT_TRUE(f.results["hog"].message.find("work on this node may use 96 MiB") != std::string::npos);
    ASSERT_FALSE(f.results["hog"].oom_at_limit); // the node's budget: what it held is a lower bound
    ASSERT_TRUE(f.results["hog"].peak_mem >= (80ll << 20));
    // The kill took that item only: the next one in the batch ran.
    ASSERT_TRUE(f.results["small"].failure == ItemFailure::None);
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
    ASSERT_TRUE(f.results["big"].peak_mem >= (190ll << 20));

    // A short item is measured as exactly as a long one: its cgroup's peak, not a sample.
    spec.items = {Fixture::Sh("quick", "python3 -c 'b = bytearray(100 << 20); b[::4096] = b\"x\" * len(b[::4096])'")};
    f.Run(spec);
    if (f.results["quick"].peak_mem < (90ll << 20))
        throw std::runtime_error("quick item measured at " + std::to_string(f.results["quick"].peak_mem >> 20) + " MiB");

    // Its CPU is counted too (wait4 on bwrap reports none of it).
    spec.items = {Fixture::Sh("busy", "python3 -c 'sum(range(60_000_000))'")};
    f.Run(spec);
    if (f.results["busy"].cpu_ms < 150)
        throw std::runtime_error("busy item measured at " + std::to_string(f.results["busy"].cpu_ms) + " ms CPU");

    // A small item is small: nothing outside its own processes is counted.
    spec.items = {Fixture::Sh("small", "sleep 1")};
    f.Run(spec);
    if (f.results["small"].peak_mem >= (64ll << 20))
        throw std::runtime_error("sleep measured at " + std::to_string(f.results["small"].peak_mem >> 20) + " MiB");
}

TEST(BatchItemStoppedForMemoryIsAnOomOfTheBudget) {
    // When work is stalled on memory the watcher stops the item that started last: it is reported
    // as out of the node's memory (not its own limit), with the budget, so it is retried alone.
    struct Restore {
        ~Restore() { zeromesh::exec::cgroup::SetUpWork(0); }
    } restore;
    Fixture f(512ull << 20);
    if (!f.Usable()) return;
    ASSERT_FALSE(f.runner.StopYoungestForMemory()); // nothing runs
    auto spec = f.Spec();
    spec.items = {Fixture::Sh("long", "sleep 30")};
    std::jthread stopper([&] {
        for (int i = 0; i < 200 && !f.runner.StopYoungestForMemory(); ++i) std::this_thread::sleep_for(20ms);
    });
    const auto start = std::chrono::steady_clock::now();
    f.Run(spec);
    ASSERT_TRUE(std::chrono::steady_clock::now() - start < 10s);
    ASSERT_TRUE(f.results["long"].failure == ItemFailure::Oom);
    ASSERT_FALSE(f.results["long"].oom_at_limit);
    ASSERT_EQ(f.results["long"].budget, 512ll << 20);
    ASSERT_TRUE(f.results["long"].message.find("stalled on memory") != std::string::npos);
}

TEST(CgroupReaperRemovesBesideTheCaller) {
    // Handing a cgroup to the reaper returns at once, even with a process still in it; the reaper
    // kills what is left and removes the cgroups, the item's before its batch's.
    const auto work = zeromesh::exec::cgroup::SetUpWork(0);
    if (work.root.empty()) {
        std::cout << "[SKIP] " << work.problem << "\n";
        return;
    }
    std::string err;
    const auto batch = zeromesh::exec::cgroup::CreateBatch(work.root, 1, 0, &err);
    ASSERT_TRUE(batch.has_value());
    const auto item = zeromesh::exec::cgroup::CreateChild(*batch, "g0", &err);
    ASSERT_TRUE(item.has_value());
    const pid_t pid = fork();
    if (pid == 0) {
        const int fd = zeromesh::exec::cgroup::OpenProcs(*item);
        if (fd < 0 || write(fd, "0", 1) != 1) _exit(1);
        signal(SIGTERM, SIG_IGN); // a teardown that does not end on its own
        for (;;) pause();
    }
    while (zeromesh::exec::cgroup::Procs(*item).empty()) std::this_thread::sleep_for(5ms);
    {
        zeromesh::exec::cgroup::Reaper reaper;
        const auto start = std::chrono::steady_clock::now();
        reaper.Remove(*item);
        reaper.Remove(*batch);
        ASSERT_TRUE(std::chrono::steady_clock::now() - start < 50ms);
    } // the destructor removes what is still queued
    ASSERT_FALSE(std::filesystem::exists(*item));
    ASSERT_FALSE(std::filesystem::exists(*batch));
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}

TEST(CgroupWaitEmptySleepsUntilTheCgroupEmpties) {
    // Waiting for an item's cgroup to empty (a sandbox still tearing down) must sleep on the
    // kernel's notification, not spin: a spinning zmd thread starves the idle-priority compiles
    // on a small machine, which slows the teardown it waits for.
    const auto work = zeromesh::exec::cgroup::SetUpWork(0);
    if (work.root.empty()) {
        std::cout << "[SKIP] " << work.problem << "\n";
        return;
    }
    std::string err;
    const auto batch = zeromesh::exec::cgroup::CreateBatch(work.root, 1, 0, &err);
    ASSERT_TRUE(batch.has_value());
    const auto item = zeromesh::exec::cgroup::CreateChild(*batch, "g0", &err);
    ASSERT_TRUE(item.has_value());
    int ready[2];
    ASSERT_EQ(pipe(ready), 0);
    const pid_t pid = fork();
    if (pid == 0) {
        close(ready[0]);
        const int fd = zeromesh::exec::cgroup::OpenProcs(*item);
        if (fd < 0 || write(fd, "0", 1) != 1) _exit(1);
        close(fd);
        if (write(ready[1], "1", 1) != 1) _exit(1);
        close(ready[1]);
        usleep(300 * 1000);
        _exit(0);
    }
    close(ready[1]);
    if (pid < 0) {
        close(ready[0]);
        zeromesh::exec::cgroup::Remove(*batch);
        ASSERT_TRUE(pid >= 0);
    }
    std::jthread reap([pid] { waitpid(pid, nullptr, 0); });
    pollfd notification{ready[0], POLLIN, 0};
    char entered = 0;
    const bool child_ready = poll(&notification, 1, 1000) > 0 && read(ready[0], &entered, 1) == 1 && entered == '1';
    close(ready[0]);
    if (!child_ready) {
        kill(pid, SIGKILL);
        reap.join();
        zeromesh::exec::cgroup::Remove(*batch);
        ASSERT_TRUE(child_ready);
    }
    auto cpu_now = [] {
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec);
    };
    const auto wall0 = std::chrono::steady_clock::now();
    const auto cpu0 = cpu_now();
    ASSERT_TRUE(zeromesh::exec::cgroup::WaitEmpty(*item, 5s));
    const auto cpu = cpu_now() - cpu0;
    const auto wall = std::chrono::steady_clock::now() - wall0;
    reap.join();
    ASSERT_TRUE(wall >= 150ms);  // it did wait for the process
    ASSERT_TRUE(cpu < 30ms);     // asleep meanwhile
    ASSERT_TRUE(zeromesh::exec::cgroup::Remove(*batch));
}
