#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "plugins/env-mirror/chunk_source.hpp"
#include "plugins/env-mirror/env_profile.hpp"
#include "plugins/env-mirror/mirror_cache.hpp"
#include "plugins/env-mirror/mirror_fuse.hpp"
#include "plugins/env-mirror/mirror_prefetch.hpp"
#include "plugins/env-mirror/mirror_reader.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

using namespace zeromesh;
using namespace zeromesh::mirror;
using testing::ReadFile;
using testing::TempDir;
using testing::WriteFile;

namespace {

using namespace std::chrono_literals;

// Counts every fetch that reaches the (pretend) requester.
class CountingSource final : public IChunkSource {
public:
    explicit CountingSource(std::shared_ptr<IChunkSource> inner) : inner_(std::move(inner)) {}

    std::optional<std::string> FetchChunk(const std::string& hash) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++calls_;
            ++per_blob_[hash];
        }
        return inner_->FetchChunk(hash);
    }

    int Calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return calls_;
    }
    int CallsFor(const std::string& hash) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = per_blob_.find(hash);
        return it == per_blob_.end() ? 0 : it->second;
    }

private:
    std::shared_ptr<IChunkSource> inner_;
    mutable std::mutex mu_;
    int calls_{0};
    std::map<std::string, int> per_blob_;
};

// An environment tree on the "requester": f0..f4 at the root, f5 under sub/. Every file has
// distinct content, so every file is its own blob.
struct Env {
    TempDir cas_dir;
    TempDir src;
    std::shared_ptr<store::CasStore> cas;
    store::Digest root;
    std::vector<store::Digest> files; // f0..f5

    Env() : cas(std::make_shared<store::CasStore>(cas_dir.path())) {
        for (int i = 0; i < 6; ++i) {
            const std::string content(100 + i * 10, static_cast<char>('a' + i));
            const std::string rel = i == 5 ? "sub/f5" : "f" + std::to_string(i);
            WriteFile(src / rel, content);
            files.push_back(store::CasStore::ComputeDigest(content));
        }
        store::MerkleBuilder builder(cas);
        root = builder.BuildTree(src.path());
    }

    std::string Name(int i) const { return i == 5 ? "sub/f5" : "f" + std::to_string(i); }
    std::string Content(int i) const { return std::string(100 + i * 10, static_cast<char>('a' + i)); }
};

struct Side {
    TempDir dir;
    std::shared_ptr<CountingSource> source;
    std::shared_ptr<MirrorCache> cache;
    std::shared_ptr<MirrorReader> reader;

    explicit Side(const Env& env, std::shared_ptr<AccessRecorder> rec = nullptr)
        : source(std::make_shared<CountingSource>(std::make_shared<CasChunkSource>(env.cas))),
          cache(std::make_shared<MirrorCache>(dir / "cache", 64 * 1024 * 1024)),
          reader(std::make_shared<MirrorReader>(source, cache)) {
        if (rec) reader->SetRecorder(std::move(rec));
    }

    // What a task does: read the first bytes of file `i` through the mirror.
    std::string ReadFileIdx(const Env& env, int i) {
        char buf[512];
        const auto& d = env.files[static_cast<size_t>(i)];
        auto n = reader->Read(d.hash, static_cast<uint64_t>(d.size_bytes), 0, buf, sizeof buf);
        if (n < 0) throw std::runtime_error("read failed");
        return std::string(buf, static_cast<size_t>(n));
    }
};

EnvFile Ef(const std::string& hash, uint64_t size) {
    return EnvFile{hash, size};
}

bool WaitFor(const std::function<bool()>& pred) {
    for (int i = 0; i < 250; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

} // namespace

// ---- recorder ------------------------------------------------------------------------

TEST(ProfileRecorderKeepsFirstTouchOrderAndDropsRepeats) {
    AccessRecorder rec;
    rec.Record("aaa", 0, 100);
    rec.Record("bbb", 0, 10);
    rec.Record("aaa", 50, 10); // same blob, same chunk: already recorded
    rec.Record("ccc", 7, 1);
    auto p = rec.Snapshot("root");
    ASSERT_EQ(p.root_hash, std::string("root"));
    ASSERT_EQ(p.entries.size(), static_cast<size_t>(3));
    ASSERT_EQ(p.entries[0].blob, std::string("aaa"));
    ASSERT_EQ(p.entries[1].blob, std::string("bbb"));
    ASSERT_EQ(p.entries[2].blob, std::string("ccc"));
    ASSERT_EQ(p.entries[2].offset, static_cast<uint64_t>(7));
}

TEST(ProfileRecorderSplitsAReadThatSpansChunks) {
    AccessRecorder rec;
    rec.Record("aaa", kProfileChunkBytes - 10, 20); // last 10 bytes of chunk 0 + first 10 of chunk 1
    auto p = rec.Snapshot("r");
    ASSERT_EQ(p.entries.size(), static_cast<size_t>(2));
    ASSERT_EQ(p.entries[0].chunk, static_cast<uint32_t>(0));
    ASSERT_EQ(p.entries[0].offset, kProfileChunkBytes - 10);
    ASSERT_EQ(p.entries[1].chunk, static_cast<uint32_t>(1));
    ASSERT_EQ(p.entries[1].offset, kProfileChunkBytes);
}

TEST(ProfileRecorderIgnoresEmptyReads) {
    AccessRecorder rec;
    rec.Record("aaa", 0, 0);
    ASSERT_EQ(rec.Snapshot("r").entries.size(), static_cast<size_t>(0));
}

// ---- serialization -------------------------------------------------------------------

TEST(ProfileRoundTripsThroughProtoPreservingOrder) {
    EnvProfile p;
    p.root_hash = std::string(64, 'a');
    p.entries = {{"b", 0, 5}, {"a", 3, 3 * kProfileChunkBytes}, {"c", 0, 0}};
    auto back = EnvProfile::Deserialize(p.Serialize());
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->root_hash, p.root_hash);
    ASSERT_EQ(back->entries.size(), static_cast<size_t>(3));
    ASSERT_EQ(back->entries[0].blob, std::string("b"));
    ASSERT_EQ(back->entries[1].blob, std::string("a"));
    ASSERT_EQ(back->entries[1].chunk, static_cast<uint32_t>(3));
    ASSERT_EQ(back->entries[1].offset, 3 * kProfileChunkBytes);
    ASSERT_EQ(back->entries[2].blob, std::string("c"));
}

TEST(ProfileDeserializeRejectsGarbage) {
    ASSERT_TRUE(!EnvProfile::Deserialize("\xff\xff\xff not a protobuf").has_value());
}

TEST(ProfileIdIsContentHashAndOrderSensitive) {
    EnvProfile a;
    a.root_hash = std::string(64, 'a');
    a.entries = {{"x", 0, 0}, {"y", 0, 0}};
    EnvProfile b = a;
    std::swap(b.entries[0], b.entries[1]);
    ASSERT_EQ(a.Id(), store::CasStore::ComputeDigest(a.Serialize()).hash);
    ASSERT_TRUE(a.Id() != b.Id());
    ASSERT_TRUE(store::CasStore::IsValidHash(a.Id()));
}

// ---- store ---------------------------------------------------------------------------

TEST(ProfileStoreSavesPerRootAndLoadsByRootAndById) {
    TempDir dir;
    ProfileStore store(dir / "profiles");
    EnvProfile p;
    p.root_hash = std::string(64, 'c');
    p.entries = {{std::string(64, '1'), 0, 0}, {std::string(64, '2'), 0, 9}};
    std::string id;
    ASSERT_TRUE(store.Save(p, &id));
    ASSERT_EQ(id, p.Id());

    auto by_root = store.LoadForRoot(p.root_hash);
    ASSERT_TRUE(by_root.has_value());
    ASSERT_EQ(by_root->entries.size(), static_cast<size_t>(2));
    ASSERT_EQ(by_root->entries[1].offset, static_cast<uint64_t>(9));

    auto by_id = store.LoadById(id);
    ASSERT_TRUE(by_id.has_value());
    ASSERT_EQ(by_id->root_hash, p.root_hash);

    ASSERT_TRUE(!store.LoadForRoot(std::string(64, 'd')).has_value());
}

TEST(ProfileStoreRefusesEmptyProfilesAndBadNames) {
    TempDir dir;
    ProfileStore store(dir / "profiles");
    EnvProfile empty;
    empty.root_hash = std::string(64, 'c');
    ASSERT_TRUE(!store.Save(empty));

    EnvProfile bad_root;
    bad_root.root_hash = "../../etc/passwd";
    bad_root.entries = {{"x", 0, 0}};
    ASSERT_TRUE(!store.Save(bad_root));

    ASSERT_TRUE(!store.LoadById("../secret").has_value());
    ASSERT_TRUE(!store.LoadForRoot("../secret").has_value());
}

TEST(ProfileStoreRejectsAFileWhoseContentDoesNotMatchItsId) {
    TempDir dir;
    ProfileStore store(dir / "profiles");
    EnvProfile p;
    p.root_hash = std::string(64, 'c');
    p.entries = {{"x", 0, 0}};
    std::string id;
    ASSERT_TRUE(store.Save(p, &id));

    EnvProfile other;
    other.root_hash = std::string(64, 'e');
    other.entries = {{"evil", 0, 0}};
    WriteFile(dir / "profiles" / id, other.Serialize()); // tamper

    ASSERT_TRUE(!store.LoadById(id).has_value());
}

// ---- plan: order and the 80% crossover ---------------------------------------------

TEST(PlanBelowCoverageThresholdFollowsProfileOrderOnly) {
    std::vector<EnvFile> env;
    for (int i = 0; i < 5; ++i) env.push_back(Ef("h" + std::to_string(i), 100));
    EnvProfile p;
    p.entries = {{"h3", 0, 0}, {"h1", 0, 0}, {"gone", 0, 0}, {"h3", 1, kProfileChunkBytes}};
    auto plan = MakePrefetchPlan(p, env);
    ASSERT_TRUE(!plan.whole);
    ASSERT_EQ(plan.blobs, (std::vector<std::string>{"h3", "h1"})); // stale + duplicate blobs dropped
    ASSERT_EQ(plan.bytes, static_cast<uint64_t>(200));
    ASSERT_TRUE(plan.coverage > 0.39 && plan.coverage < 0.41);
}

TEST(PlanAboveEightyPercentFetchesTheWholeEnvironmentInTreeOrder) {
    std::vector<EnvFile> env;
    for (int i = 0; i < 10; ++i) env.push_back(Ef("h" + std::to_string(i), 100));
    EnvProfile p;
    for (int i : {9, 2, 5, 0, 7, 1, 3, 4, 6}) p.entries.push_back({"h" + std::to_string(i), 0, 0}); // 90%
    auto plan = MakePrefetchPlan(p, env);
    ASSERT_TRUE(plan.whole);
    ASSERT_EQ(plan.blobs.size(), static_cast<size_t>(10));
    ASSERT_EQ(plan.blobs.front(), std::string("h0")); // tree order, not profile order
    ASSERT_EQ(plan.blobs.back(), std::string("h9"));
    ASSERT_EQ(plan.bytes, static_cast<uint64_t>(1000));
}

TEST(PlanExactlyEightyPercentIsNotWhole) {
    std::vector<EnvFile> env;
    for (int i = 0; i < 10; ++i) env.push_back(Ef("h" + std::to_string(i), 100));
    EnvProfile p;
    for (int i = 0; i < 8; ++i) p.entries.push_back({"h" + std::to_string(i), 0, 0}); // 80.0%
    auto plan = MakePrefetchPlan(p, env);
    ASSERT_TRUE(!plan.whole);
    ASSERT_EQ(plan.blobs.size(), static_cast<size_t>(8));
}

TEST(PlanCoverageIsByBytesNotFileCount) {
    // 1 big file out of 5 is 90% of the bytes.
    std::vector<EnvFile> env = {Ef("big", 900), Ef("a", 25), Ef("b", 25), Ef("c", 25), Ef("d", 25)};
    EnvProfile p;
    p.entries = {{"big", 0, 0}};
    ASSERT_TRUE(MakePrefetchPlan(p, env).whole);
}

TEST(PlanForAnEmptyProfileOrEnvironmentFetchesNothing) {
    EnvProfile p;
    auto plan = MakePrefetchPlan(p, {Ef("a", 10)});
    ASSERT_TRUE(!plan.whole);
    ASSERT_TRUE(plan.blobs.empty());
    p.entries = {{"a", 0, 0}};
    ASSERT_TRUE(MakePrefetchPlan(p, {}).blobs.empty());
}

// ---- reader: counters and recording --------------------------------------------------

TEST(ReaderCountsOnDemandFetchesAndCacheHits) {
    Env env;
    Side s(env);
    auto first = s.ReadFileIdx(env, 2);
    ASSERT_EQ(first, env.Content(2).substr(0, first.size()));
    ASSERT_EQ(first.size(), static_cast<size_t>(120));
    s.ReadFileIdx(env, 2); // second read is served from the cache
    auto st = s.reader->Stats();
    ASSERT_EQ(st.read_ops, static_cast<uint64_t>(2));
    ASSERT_EQ(st.on_demand_fetches, static_cast<uint64_t>(1));
    ASSERT_EQ(st.cache_hits, static_cast<uint64_t>(1));
    ASSERT_EQ(st.prefetch_fetches, static_cast<uint64_t>(0));
    ASSERT_EQ(s.source->Calls(), 1);
}

TEST(ReaderHonoursOffsetAndEndOfFile) {
    Env env;
    Side s(env);
    const auto& d = env.files[0]; // 100 bytes of 'a'
    char buf[64];
    auto n = s.reader->Read(d.hash, 100, 90, buf, sizeof buf);
    ASSERT_EQ(n, static_cast<std::ptrdiff_t>(10));
    ASSERT_EQ(std::string(buf, 10), std::string(10, 'a'));
    ASSERT_EQ(s.reader->Read(d.hash, 100, 100, buf, sizeof buf), static_cast<std::ptrdiff_t>(0));
}

TEST(ReaderFailsWhenTheSourceCannotProvideTheBlob) {
    Env env;
    Side s(env);
    char buf[16];
    ASSERT_TRUE(s.reader->Read(std::string(64, 'f'), 16, 0, buf, sizeof buf) < 0);
}

TEST(ReaderRecordsTheOrderTheTaskTouchedFiles) {
    Env env;
    auto rec = std::make_shared<AccessRecorder>();
    Side s(env, rec);
    s.ReadFileIdx(env, 4);
    s.ReadFileIdx(env, 1);
    s.ReadFileIdx(env, 4);
    auto p = rec->Snapshot(env.root.hash);
    ASSERT_EQ(p.entries.size(), static_cast<size_t>(2));
    ASSERT_EQ(p.entries[0].blob, env.files[4].hash);
    ASSERT_EQ(p.entries[1].blob, env.files[1].hash);
}

// ---- prefetch: the point of all this ------------------------------------------------

TEST(CollectEnvFilesWalksTheWholeTreeWithoutCountingOnDemandFetches) {
    Env env;
    Side s(env);
    auto files = CollectEnvFiles(*s.reader, env.root.hash);
    ASSERT_EQ(files.size(), static_cast<size_t>(6)); // includes sub/f5
    uint64_t total = 0;
    for (const auto& f : files) total += f.size;
    ASSERT_EQ(total, static_cast<uint64_t>(100 + 110 + 120 + 130 + 140 + 150));
    ASSERT_EQ(s.reader->Stats().on_demand_fetches, static_cast<uint64_t>(0));
    ASSERT_TRUE(s.reader->Stats().prefetch_fetches >= 2); // root manifest + sub manifest
}

TEST(PrefetchWithAProfileLeavesNoOnDemandFetchesForTheRecordedFiles) {
    Env env;

    // Run 1: no profile. The task reads f0, f2, f4; each is a round trip. A recorder watches.
    auto rec = std::make_shared<AccessRecorder>();
    Side first(env, rec);
    for (int i : {0, 2, 4}) first.ReadFileIdx(env, i);
    ASSERT_EQ(first.reader->Stats().on_demand_fetches, static_cast<uint64_t>(3));
    EnvProfile profile = rec->Snapshot(env.root.hash);

    // Run 2: a fresh donor cache, same task, with the profile prefetched before it starts.
    Side second(env);
    auto plan = MakePrefetchPlan(profile, CollectEnvFiles(*second.reader, env.root.hash));
    ASSERT_TRUE(!plan.whole); // 3 of 6 files: stay selective
    ASSERT_EQ(plan.blobs.size(), static_cast<size_t>(3));
    ASSERT_EQ(RunPrefetch(plan, *second.reader, std::stop_token{}), static_cast<size_t>(3));

    for (int i : {0, 2, 4}) {
        ASSERT_EQ(second.ReadFileIdx(env, i), first.ReadFileIdx(env, i)); // same bytes either way
    }
    auto st = second.reader->Stats();
    ASSERT_EQ(st.on_demand_fetches, static_cast<uint64_t>(0));
    ASSERT_TRUE(st.on_demand_fetches < first.reader->Stats().on_demand_fetches);
    ASSERT_EQ(st.prefetch_fetches >= 3, true);
    // The unprofiled file is still fetched lazily, once.
    second.ReadFileIdx(env, 1);
    ASSERT_EQ(second.reader->Stats().on_demand_fetches, static_cast<uint64_t>(1));
}

TEST(PrefetchFetchesTheProfiledBlobsInProfileOrder) {
    Env env;
    Side s(env);
    EnvProfile profile;
    profile.root_hash = env.root.hash;
    profile.entries = {{env.files[3].hash, 0, 0}, {env.files[0].hash, 0, 0}};
    std::vector<std::string> order;
    auto plan = MakePrefetchPlan(profile, CollectEnvFiles(*s.reader, env.root.hash));
    ASSERT_EQ(plan.blobs, (std::vector<std::string>{env.files[3].hash, env.files[0].hash}));
    RunPrefetch(plan, *s.reader, std::stop_token{}, [&](const std::string& h) { order.push_back(h); });
    ASSERT_EQ(order, plan.blobs);
}

TEST(PrefetchWholeEnvironmentWhenTheProfileCoversMostOfIt) {
    Env env;
    auto rec = std::make_shared<AccessRecorder>();
    Side first(env, rec);
    for (int i = 0; i < 6; ++i) first.ReadFileIdx(env, i);
    Side second(env);
    auto plan = MakePrefetchPlan(rec->Snapshot(env.root.hash), CollectEnvFiles(*second.reader, env.root.hash));
    ASSERT_TRUE(plan.whole);
    ASSERT_EQ(RunPrefetch(plan, *second.reader, std::stop_token{}), static_cast<size_t>(6));
    for (int i = 0; i < 6; ++i) second.ReadFileIdx(env, i);
    ASSERT_EQ(second.reader->Stats().on_demand_fetches, static_cast<uint64_t>(0));
}

TEST(PrefetchSkipsBlobsAlreadyCached) {
    Env env;
    Side s(env);
    s.ReadFileIdx(env, 0); // f0 is now cached
    PrefetchPlan plan;
    plan.blobs = {env.files[0].hash, env.files[1].hash};
    ASSERT_EQ(RunPrefetch(plan, *s.reader, std::stop_token{}), static_cast<size_t>(1));
    ASSERT_EQ(s.source->CallsFor(env.files[0].hash), 1);
    ASSERT_EQ(s.source->CallsFor(env.files[1].hash), 1);
}

TEST(PrefetchStopsWhenAskedTo) {
    Env env;
    Side s(env);
    PrefetchPlan plan;
    for (int i = 0; i < 6; ++i) plan.blobs.push_back(env.files[static_cast<size_t>(i)].hash);
    std::stop_source stop;
    stop.request_stop();
    ASSERT_EQ(RunPrefetch(plan, *s.reader, stop.get_token()), static_cast<size_t>(0));
    ASSERT_EQ(s.source->Calls(), 0);
}

TEST(BackgroundPrefetchWarmsTheCacheWhileTheCallerContinues) {
    Env env;
    EnvProfile profile;
    profile.root_hash = env.root.hash;
    profile.entries = {{env.files[1].hash, 0, 0}, {env.files[2].hash, 0, 0}};
    Side s(env);
    {
        BackgroundPrefetch bg(profile, s.reader);
        ASSERT_TRUE(WaitFor([&] { return s.cache->Contains(env.files[1].hash) && s.cache->Contains(env.files[2].hash); }));
    } // destructor joins
    s.ReadFileIdx(env, 1);
    ASSERT_EQ(s.reader->Stats().on_demand_fetches, static_cast<uint64_t>(0));
}

// ---- through a real FUSE mount (skips without /dev/fuse) ------------------------------

TEST(MirrorFuseExposesRoundTripCountsAndPrefetchReducesThem) {
    if (!MirrorFuse::IsFuseAvailable()) {
        std::cout << "[SKIP] FUSE not available (/dev/fuse not accessible)\n";
        return;
    }
    Env env;
    EnvProfile profile;
    uint64_t first_run_fetches = 0;

    {   // First run: record.
        TempDir mnt;
        auto source = std::make_shared<CountingSource>(std::make_shared<CasChunkSource>(env.cas));
        auto cache = std::make_shared<MirrorCache>(mnt / "cache", 64 * 1024 * 1024);
        MirrorFuse fuse(mnt / "m", source, cache);
        auto rec = std::make_shared<AccessRecorder>();
        fuse.SetRecorder(rec);
        ASSERT_TRUE(fuse.Mount(env.root.hash));
        ASSERT_EQ(ReadFile(mnt / "m" / "f0"), env.Content(0));
        ASSERT_EQ(ReadFile(mnt / "m" / "f3"), env.Content(3));
        auto st = fuse.Stats();
        ASSERT_TRUE(st.read_ops >= 2);
        ASSERT_TRUE(st.on_demand_fetches >= 3); // root manifest + f0 + f3
        first_run_fetches = st.on_demand_fetches;
        profile = rec->Snapshot(env.root.hash);
        fuse.Unmount();
    }
    ASSERT_EQ(profile.entries.size(), static_cast<size_t>(2));
    ASSERT_EQ(profile.entries[0].blob, env.files[0].hash);
    ASSERT_EQ(profile.entries[1].blob, env.files[3].hash);

    {   // Second run: same reads, profile prefetched in the background.
        TempDir mnt;
        auto source = std::make_shared<CountingSource>(std::make_shared<CasChunkSource>(env.cas));
        auto cache = std::make_shared<MirrorCache>(mnt / "cache", 64 * 1024 * 1024);
        MirrorFuse fuse(mnt / "m", source, cache);
        ASSERT_TRUE(fuse.Mount(env.root.hash));
        BackgroundPrefetch bg(profile, fuse.Reader());
        ASSERT_TRUE(WaitFor([&] { return cache->Contains(env.files[0].hash) && cache->Contains(env.files[3].hash); }));
        ASSERT_EQ(ReadFile(mnt / "m" / "f0"), env.Content(0));
        ASSERT_EQ(ReadFile(mnt / "m" / "f3"), env.Content(3));
        ASSERT_TRUE(fuse.Stats().on_demand_fetches < first_run_fetches);
        ASSERT_EQ(fuse.Stats().on_demand_fetches, static_cast<uint64_t>(1)); // only the root manifest
        fuse.Unmount();
    }
}
