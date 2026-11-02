#include "run/run_engine.hpp"

#include "node/convert.hpp"
#include "node/env_check.hpp"
#include "run/item_key.hpp"
#include "run/place.hpp"

#include <google/protobuf/util/json_util.h>
#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace zeromesh::run {

namespace {

namespace fs = std::filesystem;

constexpr size_t kLookupChunk = 1024;
constexpr size_t kMaxItemsPerBatch = 2000;
constexpr double kMinBatchSeconds = 2.0;
constexpr double kMaxBatchSeconds = 120.0;
constexpr double kDefaultItemSeconds = 1.0;
constexpr int kMaxOomRetries = 2;
constexpr int64_t kMiB = 1024 * 1024;
constexpr size_t kFinishedRunsKept = 32;
constexpr auto kTeamTimeout = std::chrono::milliseconds(2000);
constexpr auto kOutputGrace = std::chrono::minutes(10);
constexpr auto kHistoryKept = std::chrono::hours(24 * 30);
constexpr size_t kMaxIdBytes = 200;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string RandomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    randombytes_buf(raw.data(), raw.size());
    std::string out;
    for (unsigned char c : raw) {
        out.push_back("0123456789abcdef"[c >> 4]);
        out.push_back("0123456789abcdef"[c & 15]);
    }
    return out;
}

bool IsTerminal(v1::ItemState s) {
    return s == v1::ITEM_SUCCEEDED || s == v1::ITEM_FAILED || s == v1::ITEM_CANCELLED;
}

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > kMaxIdBytes) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return c > 0x20 && c < 0x7f; });
}

// Policy with every default filled in.
v1::Policy Normalized(v1::Policy p) {
    if (!p.has_infra_retries()) p.set_infra_retries(3);
    if (!p.has_exit_retries()) p.set_exit_retries(0);
    if (!p.has_timeout_s() || p.timeout_s() <= 0) p.set_timeout_s(3600);
    if (!p.has_hung_after_s()) p.set_hung_after_s(600);
    if (!p.has_keep_going()) p.set_keep_going(false);
    if (!p.has_local_fallback()) p.set_local_fallback(true);
    if (!p.has_cache()) p.set_cache(true);
    if (!p.has_batch()) p.set_batch(0);
    return p;
}

std::string OutputKey(const std::string& path) {
    return fs::path(path).lexically_normal().string();
}

bool WriteFileAtomically(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp-" + RandomHex(4);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << data;
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string ToJson(const google::protobuf::Message& m) {
    std::string out;
    google::protobuf::util::JsonPrintOptions opts;
    opts.preserve_proto_field_names = true;
    opts.add_whitespace = true;
    (void)google::protobuf::util::MessageToJsonString(m, &out, opts);
    return out;
}

} // namespace

// ---- state ------------------------------------------------------------------------------------

struct RunEngine::Item {
    v1::Item spec;
    size_t stage{0};
    v1::ItemState state{v1::ITEM_WAITING};
    std::vector<size_t> deps;
    std::vector<size_t> dependents;
    store::Digest root; // the workspace with the outputs of the items it comes after
    bool root_ready{false};
    bool looked_up{false};
    bool in_lookup{false};
    std::string weak;
    int infra_failures{0};
    int exit_failures{0};
    int oom_retries{0};
    bool local_only{false};
    bool fallback_tried{false};
    std::set<std::string> exclude;
    int64_t mem_override{0};
    v1::Digest checkpoint;
    std::string batch; // the batch it is queued or running in
    std::string node;
    int64_t started_ms{0};
    v1::ItemResult result;
};

struct BatchRun {
    bool cancel_sent{false};
    std::vector<size_t> items;
    std::set<size_t> reported;
    std::string node;
    int tid{-1};
    int cores{1};
    int64_t started_ms{0};
};

struct RunEngine::Run {
    std::string id;
    v1::RunSpec spec; // as submitted, items included (for the history and Resume)
    v1::Policy policy;
    std::vector<v1::Stage> stages; // exact programs stamped
    std::vector<std::string> env_ids;
    std::map<std::string, size_t> stage_index;
    std::vector<Item> items;
    std::unordered_map<std::string, size_t> index;
    bool open{false};
    bool cancelled{false};
    bool done{false};
    std::string ended_by;
    int64_t started_ms{0};
    int64_t finished_ms{0};
    std::chrono::steady_clock::time_point finished_at{};
    std::vector<v1::RunEvent> history;
    std::vector<std::shared_ptr<RunEvents>> watchers;
    std::map<std::string, BatchRun> batches;
    int batch_seq{0};
    uint64_t pushed{0}, pulled{0}, seeded{0};
    double core_s{0};
    std::map<std::string, v1::NodeShare> nodes;
    std::map<std::string, std::vector<bool>> busy_tids; // node -> timeline lanes in use
    std::vector<v1::TraceEvent> timeline;
};

// ---- lifecycle --------------------------------------------------------------------------------

RunEngine::RunEngine(EngineDeps deps) : deps_(std::move(deps)) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium initialisation failed");
    if (!deps_.cas || !deps_.merkle || !deps_.cache || !deps_.executor || !deps_.resources) {
        throw std::invalid_argument("RunEngine: missing dependency");
    }
    std::error_code ec;
    fs::create_directories(deps_.runs_dir, ec);
    const auto cutoff = fs::file_time_type::clock::now() - kHistoryKept;
    for (fs::directory_iterator it(deps_.runs_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->last_write_time(ec) < cutoff) fs::remove(it->path(), ec);
    }
    loop_ = std::jthread([this](std::stop_token st) { Loop(st); });
}

RunEngine::~RunEngine() {
    std::vector<std::string> ids;
    {
        std::lock_guard lock(mu_);
        for (const auto& [id, r] : runs_) {
            if (!r->done) ids.push_back(id);
        }
    }
    for (const auto& id : ids) Cancel(id);
    Wake();
    // Let the loop send the cancels, then stop it.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    loop_.request_stop();
    Wake();
    loop_ = {};
    std::unique_lock lock(guard_->mu);
    guard_->dead = true;
}

void RunEngine::Wake() {
    {
        std::lock_guard lock(mu_);
        dirty_ = true;
    }
    cv_.notify_all();
}

void RunEngine::Loop(std::stop_token st) {
    while (!st.stop_requested()) {
        std::vector<RunPtr> runs;
        {
            std::unique_lock lock(mu_);
            cv_.wait_for(lock, st, std::chrono::milliseconds(200), [this] { return dirty_; });
            dirty_ = false;
            for (const auto& id : order_) {
                auto it = runs_.find(id);
                if (it != runs_.end() && !it->second->done) runs.push_back(it->second);
            }
        }
        for (const auto& r : runs) {
            if (st.stop_requested()) break;
            try {
                Pump(r);
            } catch (const std::exception& e) {
                std::lock_guard lock(mu_);
                v1::RunEvent ev;
                ev.mutable_notice()->set_text(std::string("engine: ") + e.what());
                Emit(*r, std::move(ev));
            }
        }
    }
}

// ---- submitting -------------------------------------------------------------------------------

std::string RunEngine::Submit(v1::RunSpec spec) {
    auto run = std::make_shared<Run>();
    run->policy = Normalized(spec.policy());
    run->open = spec.open();
    run->started_ms = NowMs();
    if (!spec.workspace().hash().empty()) {
        if (!store::CasStore::IsValidHash(spec.workspace().hash()) || !deps_.cas->HasBlob(spec.workspace().hash())) {
            throw std::invalid_argument("workspace " + spec.workspace().hash() + " is not in this node's store");
        }
    }
    if (!spec.report_path().empty() && !fs::path(spec.report_path()).is_absolute()) {
        throw std::invalid_argument("report_path must be absolute");
    }
    if (spec.stages().empty()) spec.add_stages(); // the default stage, ""
    for (const auto& s : spec.stages()) {
        if (run->stage_index.contains(s.name())) throw std::invalid_argument("duplicate stage '" + s.name() + "'");
        v1::Stage stage = s;
        node::StampPrograms(*stage.mutable_env());
        if (stage.resources().arch().empty()) stage.mutable_resources()->set_arch(deps_.arch);
        run->stage_index[s.name()] = run->stages.size();
        run->env_ids.push_back(node::EnvIdentity(stage.env()));
        run->stages.push_back(std::move(stage));
    }
    std::vector<v1::Item> items(spec.items().begin(), spec.items().end());
    spec.clear_items();
    run->spec = std::move(spec);

    std::lock_guard lock(mu_);
    do {
        run->id = RandomHex(6);
    } while (runs_.contains(run->id));
    AddItemsLocked(*run, std::move(items)); // throws before the run is registered
    runs_[run->id] = run;
    order_.push_back(run->id);
    WriteFileAtomically(deps_.runs_dir / (run->id + ".spec"), [&] {
        v1::RunSpec saved = run->spec;
        for (const auto& it : run->items) *saved.add_items() = it.spec;
        saved.set_open(false);
        return saved.SerializeAsString();
    }());
    dirty_ = true;
    cv_.notify_all();
    return run->id;
}

void RunEngine::AddItemsLocked(Run& run, std::vector<v1::Item> items) {
    // Validate everything before changing anything.
    std::unordered_map<std::string, size_t> fresh;
    for (size_t k = 0; k < items.size(); ++k) {
        const v1::Item& it = items[k];
        if (!ValidId(it.id())) throw std::invalid_argument("invalid item id '" + it.id() + "'");
        if (run.index.contains(it.id()) || !fresh.emplace(it.id(), k).second) {
            throw std::invalid_argument("duplicate item id '" + it.id() + "'");
        }
    }
    for (size_t k = 0; k < items.size(); ++k) {
        v1::Item& it = items[k];
        auto st = run.stage_index.find(it.stage());
        if (st == run.stage_index.end()) throw std::invalid_argument("item '" + it.id() + "': unknown stage '" + it.stage() + "'");
        const v1::Stage& stage = run.stages[st->second];
        if (it.argv().empty() && stage.combine().argv().empty()) {
            throw std::invalid_argument("item '" + it.id() + "' has no command");
        }
        try {
            const std::string cwd = WorkspacePath("", it.cwd());
            for (const auto& o : it.outputs()) {
                if (WorkspacePath(cwd, o.path()) == cwd) throw std::invalid_argument("empty output path");
                if (!o.dest().empty() && !fs::path(o.dest()).is_absolute()) {
                    throw std::invalid_argument("output destination must be absolute: " + o.dest());
                }
            }
            for (const auto& i : it.inputs()) WorkspacePath("", i);
            if (!it.depfile().empty()) WorkspacePath(cwd, it.depfile());
            for (const auto& d : it.search_dirs()) WorkspacePath(cwd, d);
        } catch (const std::invalid_argument& e) {
            throw std::invalid_argument("item '" + it.id() + "': " + e.what());
        }
        if (!it.changed_dest().empty() && !fs::path(it.changed_dest()).is_absolute()) {
            throw std::invalid_argument("item '" + it.id() + "': changed_dest must be absolute");
        }
        for (const auto& a : it.after()) {
            if (!run.index.contains(a) && !fresh.contains(a)) {
                throw std::invalid_argument("item '" + it.id() + "' comes after unknown item '" + a + "'");
            }
        }
    }
    // Cycles can only run through the new items (old ones never point at new ones).
    {
        std::vector<int> indeg(items.size(), 0);
        std::vector<std::vector<size_t>> out(items.size());
        for (size_t k = 0; k < items.size(); ++k) {
            for (const auto& a : items[k].after()) {
                if (auto f = fresh.find(a); f != fresh.end()) {
                    out[f->second].push_back(k);
                    ++indeg[k];
                }
            }
        }
        std::vector<size_t> frontier;
        for (size_t k = 0; k < items.size(); ++k) {
            if (indeg[k] == 0) frontier.push_back(k);
        }
        size_t seen = 0;
        while (!frontier.empty()) {
            size_t n = frontier.back();
            frontier.pop_back();
            ++seen;
            for (size_t m : out[n]) {
                if (--indeg[m] == 0) frontier.push_back(m);
            }
        }
        if (seen != items.size()) throw std::invalid_argument("items depend on each other in a cycle");
    }

    const size_t base = run.items.size();
    for (auto& spec : items) {
        Item it;
        it.stage = run.stage_index.at(spec.stage());
        it.result.set_id(spec.id());
        it.result.set_state(v1::ITEM_WAITING);
        it.spec = std::move(spec);
        run.index[it.spec.id()] = run.items.size();
        run.items.push_back(std::move(it));
    }
    for (size_t i = base; i < run.items.size(); ++i) {
        Item& it = run.items[i];
        std::set<size_t> deps;
        for (const auto& a : it.spec.after()) deps.insert(run.index.at(a));
        for (size_t d : deps) {
            it.deps.push_back(d);
            run.items[d].dependents.push_back(i);
        }
        v1::RunEvent ev;
        auto* u = ev.mutable_update();
        u->set_id(it.spec.id());
        u->set_state(v1::ITEM_WAITING);
        Emit(run, std::move(ev));
    }
}

void RunEngine::AddItems(const std::string& run_id, std::vector<v1::Item> items) {
    std::lock_guard lock(mu_);
    auto it = runs_.find(run_id);
    if (it == runs_.end()) throw std::invalid_argument("unknown run '" + run_id + "'");
    Run& run = *it->second;
    if (!run.open || run.done) throw std::invalid_argument("run '" + run_id + "' is not open");
    AddItemsLocked(run, std::move(items));
    dirty_ = true;
    cv_.notify_all();
}

void RunEngine::Close(const std::string& run_id) {
    std::lock_guard lock(mu_);
    auto it = runs_.find(run_id);
    if (it == runs_.end()) throw std::invalid_argument("unknown run '" + run_id + "'");
    Run& run = *it->second;
    run.open = false;
    v1::RunSpec saved = run.spec;
    for (const auto& item : run.items) *saved.add_items() = item.spec;
    saved.set_open(false);
    WriteFileAtomically(deps_.runs_dir / (run.id + ".spec"), saved.SerializeAsString());
    dirty_ = true;
    cv_.notify_all();
}

std::string RunEngine::Resume(const std::string& run_id) {
    if (run_id.empty() || run_id.find('/') != std::string::npos || run_id.find('.') != std::string::npos) {
        throw std::invalid_argument("invalid run id");
    }
    std::ifstream in(deps_.runs_dir / (run_id + ".spec"), std::ios::binary);
    if (!in) throw std::invalid_argument("no record of run '" + run_id + "'");
    std::stringstream ss;
    ss << in.rdbuf();
    v1::RunSpec spec;
    if (!spec.ParseFromString(ss.str())) throw std::invalid_argument("the record of run '" + run_id + "' is corrupt");
    spec.set_open(false);
    return Submit(std::move(spec));
}

// ---- cancel -----------------------------------------------------------------------------------

void RunEngine::Cancel(const std::string& run_id) {
    {
        std::lock_guard lock(mu_);
        auto it = runs_.find(run_id);
        if (it == runs_.end() || !it->second) return;
        Run& run = *it->second;
        if (run.done) return;
        CancelLocked(run, "cancel");
        dirty_ = true;
    }
    cv_.notify_all(); // the pump stops the run's batches (never under mu_: a cancel may call back)
}

void RunEngine::CancelLocked(Run& run, const std::string& ended_by) {
    if (run.cancelled) return;
    run.cancelled = true;
    run.open = false;
    if (run.ended_by.empty()) run.ended_by = ended_by;
    for (size_t i = 0; i < run.items.size(); ++i) {
        Item& it = run.items[i];
        if (IsTerminal(it.state) || !it.batch.empty()) continue; // running ones end with their batch
        v1::Failure f;
        f.set_kind(v1::FAILURE_CANCELLED);
        f.set_message(ended_by == "failure" ? "another item failed" : "run ended: " + ended_by);
        Terminal(run, i, v1::ITEM_CANCELLED, f);
    }
}

// ---- events -----------------------------------------------------------------------------------

void RunEngine::Emit(Run& run, v1::RunEvent ev, bool keep) {
    ev.set_run_id(run.id);
    std::erase_if(run.watchers, [](const auto& w) { return w->closed(); });
    for (auto& w : run.watchers) w->Push(ev);
    if (keep) run.history.push_back(std::move(ev));
}

void RunEngine::Update(Run& run, size_t i, v1::ItemState state, const std::string& node, const std::string& detail) {
    Item& it = run.items[i];
    it.state = state;
    it.result.set_state(state);
    v1::RunEvent ev;
    auto* u = ev.mutable_update();
    u->set_id(it.spec.id());
    u->set_state(state);
    u->set_node(node);
    if (!node.empty() && deps_.hostname) u->set_hostname(deps_.hostname(node));
    u->set_attempt(static_cast<int32_t>(it.result.attempts_size()) + 1);
    u->set_detail(detail);
    Emit(run, std::move(ev));
}

void RunEngine::Terminal(Run& run, size_t i, v1::ItemState state, v1::Failure failure) {
    Item& it = run.items[i];
    if (IsTerminal(it.state)) return;
    it.state = state;
    it.batch.clear();
    it.result.set_state(state);
    if (failure.kind() != v1::FAILURE_NONE) *it.result.mutable_failure() = std::move(failure);
    v1::RunEvent ev;
    *ev.mutable_result() = it.result;
    Emit(run, std::move(ev));
    if (state == v1::ITEM_FAILED && !run.policy.keep_going()) CancelLocked(run, "failure");
}

void RunEngine::Requeue(Run& run, size_t i, const std::string& why) {
    Item& it = run.items[i];
    it.batch.clear();
    Update(run, i, v1::ITEM_QUEUED, {}, why);
}

void RunEngine::Fail(Run& run, size_t i, v1::FailureKind kind, const std::string& message, int exit_code,
                     const std::string& out_tail, const std::string& err_tail) {
    v1::Failure f;
    f.set_kind(kind);
    f.set_message(message);
    f.set_exit_code(exit_code);
    f.set_stdout_tail(out_tail);
    f.set_stderr_tail(err_tail);
    Terminal(run, i, v1::ITEM_FAILED, std::move(f));
}

// ---- the pump ---------------------------------------------------------------------------------

bool RunEngine::Pump(const RunPtr& run) {
    struct Resolve {
        size_t i;
        store::Digest base;
        std::vector<std::pair<std::string, v1::OutputFile>> overlays; // workspace path, output
    };
    std::vector<Resolve> resolve;
    std::vector<size_t> lookup;
    std::vector<std::string> stop;
    {
        std::lock_guard lock(mu_);
        if (run->done) return false;
        const int64_t now = NowMs();
        if (!run->cancelled && run->policy.has_deadline_unix_ms() && run->policy.deadline_unix_ms() > 0 &&
            now >= run->policy.deadline_unix_ms()) {
            CancelLocked(*run, "deadline");
        }
        if (!run->cancelled && run->policy.has_budget_core_s() && run->policy.budget_core_s() > 0 &&
            run->core_s >= run->policy.budget_core_s()) {
            CancelLocked(*run, "budget");
        }
        if (run->cancelled) {
            for (auto& [id, b] : run->batches) {
                if (!b.cancel_sent) {
                    b.cancel_sent = true;
                    stop.push_back(run->id + "/" + id);
                }
            }
        }
        for (size_t i = 0; i < run->items.size(); ++i) {
            Item& it = run->items[i];
            if (it.state != v1::ITEM_WAITING) continue;
            bool ready = true, broken = false;
            std::string broken_by;
            for (size_t d : it.deps) {
                const auto s = run->items[d].state;
                if (s == v1::ITEM_FAILED || s == v1::ITEM_CANCELLED) {
                    broken = true;
                    broken_by = run->items[d].spec.id();
                }
                ready = ready && s == v1::ITEM_SUCCEEDED;
            }
            if (broken) {
                v1::Failure f;
                f.set_kind(v1::FAILURE_DEPENDENCY);
                f.set_message("item '" + broken_by + "' it comes after did not succeed");
                Terminal(*run, i, v1::ITEM_FAILED, std::move(f));
                continue;
            }
            if (!ready || run->cancelled) continue;
            Resolve r{i, node::FromProto(run->spec.workspace()), {}};
            for (size_t d : it.deps) {
                const Item& dep = run->items[d];
                for (const auto& o : dep.result.outputs()) {
                    r.overlays.emplace_back(WorkspacePath(WorkspacePath("", dep.spec.cwd()), o.path()), o);
                }
            }
            resolve.push_back(std::move(r));
            it.state = v1::ITEM_QUEUED; // claimed: its root is computed below
        }
    }
    for (const auto& b : stop) deps_.executor->Cancel(b);
    for (auto& r : resolve) {
        store::Digest root = r.base;
        for (const auto& [path, o] : r.overlays) {
            if (o.symlink() || path.empty()) continue;
            root = deps_.merkle->Overlay(root, path, node::FromProto(o.digest()), o.is_dir(), o.executable());
        }
        std::lock_guard lock(mu_);
        Item& it = run->items[r.i];
        if (IsTerminal(it.state)) continue; // cancelled meanwhile
        it.root = root;
        it.root_ready = true;
        Update(*run, r.i, v1::ITEM_QUEUED);
    }
    {
        std::lock_guard lock(mu_);
        for (size_t i = 0; i < run->items.size() && lookup.size() < kLookupChunk; ++i) {
            Item& it = run->items[i];
            if (it.state == v1::ITEM_QUEUED && it.root_ready && !it.looked_up && !it.in_lookup && !run->cancelled) {
                it.in_lookup = true;
                lookup.push_back(i);
            }
        }
    }
    if (!lookup.empty()) Lookup(run, std::move(lookup));
    Dispatch(run);

    bool finished = false;
    {
        std::lock_guard lock(mu_);
        if (!run->done && !run->open && run->batches.empty()) {
            finished = std::all_of(run->items.begin(), run->items.end(), [](const Item& it) { return IsTerminal(it.state); });
        }
    }
    if (finished) Finish(run);
    return true;
}

// ---- the cache --------------------------------------------------------------------------------

void RunEngine::Lookup(const RunPtr& run, std::vector<size_t> idx) {
    struct Probe {
        size_t i;
        v1::Item item;
        v1::Stage stage;
        store::Digest root;
        std::string weak;
        KeyMode mode;
        std::string key;                 // whole/inputs
        v1::Footprint observed;          // whole/inputs
        std::optional<v1::CachedItem> hit;
        v1::Footprint hit_footprint;     // what a depfile hit was keyed by
        std::string hit_key;
        std::string source;              // node the hit came from ("" = here)
        std::string reason;
        std::vector<std::string> changed;
        std::vector<v1::OutputFile> placed;
        bool place_failed{false};
    };
    std::vector<Probe> probes;
    bool use_cache = true;
    {
        std::lock_guard lock(mu_);
        use_cache = run->policy.cache();
        for (size_t i : idx) {
            Item& it = run->items[i];
            Probe p;
            p.i = i;
            p.item = it.spec;
            p.stage = run->stages[it.stage];
            p.root = it.root;
            if (it.weak.empty()) it.weak = WeakKey(it.spec, p.stage, run->env_ids[it.stage], p.stage.resources().arch());
            p.weak = it.weak;
            p.mode = ModeOf(it.spec);
            probes.push_back(std::move(p));
        }
    }
    auto& merkle = *deps_.merkle;
    auto& cache = *deps_.cache;
    if (use_cache) {
        // This node first.
        for (auto& p : probes) {
            if (p.mode != KeyMode::Depfile) {
                p.observed = Observe(merkle, p.root, p.item);
                p.key = FullKey(p.weak, p.observed);
                p.hit = cache.Get(p.key);
                if (p.hit) p.hit_key = p.key;
            } else {
                for (const auto& fp : cache.Footprints(p.weak)) {
                    const v1::Footprint now = Revalue(merkle, p.root, fp);
                    const std::string key = FullKey(p.weak, now);
                    if ((p.hit = cache.Get(key))) {
                        p.hit_key = key;
                        p.hit_footprint = now;
                        break;
                    }
                }
            }
        }
        // Then the team.
        std::vector<Probe*> misses;
        for (auto& p : probes) {
            if (!p.hit) misses.push_back(&p);
        }
        if (deps_.team && !misses.empty()) {
            v1::CacheLookup req;
            for (auto* p : misses) {
                if (p->mode == KeyMode::Depfile)
                    req.add_weaks(p->weak);
                else
                    req.add_keys(p->key);
            }
            std::map<std::string, std::pair<std::string, v1::CachedItem>> hits; // key -> (node, item)
            std::map<std::string, std::vector<std::pair<std::string, v1::Footprint>>> fps; // weak -> (node, fp)
            for (auto& [node, reply] : deps_.team->Lookup(req)) {
                for (const auto& h : reply.hits()) hits.emplace(h.key(), std::make_pair(node, h.item()));
                for (const auto& w : reply.footprints()) {
                    for (const auto& f : w.footprints()) fps[w.weak()].emplace_back(node, f);
                }
            }
            // Depfile items: compute the keys the peers' footprints give here, then ask for those.
            std::map<std::string, v1::CacheLookup> second; // node -> keys
            std::map<std::string, std::pair<Probe*, v1::Footprint>> candidates; // key -> probe, footprint
            for (auto* p : misses) {
                if (p->mode != KeyMode::Depfile) continue;
                for (const auto& [node, fp] : fps[p->weak]) {
                    v1::Footprint now = Revalue(merkle, p->root, fp);
                    const std::string key = FullKey(p->weak, now);
                    if (candidates.emplace(key, std::make_pair(p, now)).second) second[node].add_keys(key);
                }
            }
            for (auto& [node, req2] : second) {
                if (auto reply = deps_.team->LookupAt(node, req2)) {
                    for (const auto& h : reply->hits()) hits.emplace(h.key(), std::make_pair(node, h.item()));
                }
            }
            auto take = [&](Probe* p, const std::string& key, const v1::Footprint& fp) {
                auto h = hits.find(key);
                if (h == hits.end()) return false;
                const auto& [node, item] = h->second;
                std::vector<v1::Digest> blobs{item.stdout(), item.stderr()}, trees;
                for (const auto& o : item.outputs()) {
                    if (o.symlink()) continue;
                    (o.is_dir() ? trees : blobs).push_back(o.digest());
                }
                std::erase_if(blobs, [](const v1::Digest& d) { return d.hash().empty(); });
                if (!deps_.team->Pull(node, blobs, trees)) return false;
                cache.Put(key, item);
                cache.AddFootprint(p->weak, fp);
                p->hit = item;
                p->hit_key = key;
                p->hit_footprint = fp;
                p->source = node;
                return true;
            };
            for (auto* p : misses) {
                if (p->mode != KeyMode::Depfile) {
                    take(p, p->key, p->observed);
                    continue;
                }
                for (const auto& [key, cand] : candidates) {
                    if (cand.first == p && take(p, key, cand.second)) break;
                }
            }
        }
        // Why each miss missed: what changed since the last recorded run of the command.
        for (auto& p : probes) {
            if (p.hit) continue;
            const auto fps = cache.Footprints(p.weak);
            if (fps.empty()) {
                p.reason = "no earlier run of this command";
                continue;
            }
            const v1::Footprint now = p.mode == KeyMode::Depfile ? Revalue(merkle, p.root, fps.front()) : p.observed;
            p.changed = Changed(merkle, fps.front(), now, 10);
            p.reason = p.changed.empty() ? "the earlier result is no longer stored" : "inputs changed";
        }
    } else {
        for (auto& p : probes) p.reason = "cache off";
    }

    // Place every hit at its destinations; a hit that cannot be placed runs instead.
    for (auto& p : probes) {
        if (!p.hit) continue;
        std::map<std::string, std::string> dest;
        for (const auto& o : p.item.outputs()) dest[OutputKey(o.path())] = o.dest();
        for (auto o : p.hit->outputs()) {
            std::string err;
            if (auto d = dest.find(OutputKey(o.path())); d != dest.end()) {
                if (!d->second.empty()) {
                    if (!Place(*deps_.cas, merkle, o, d->second, &err)) p.place_failed = true;
                    o.set_placed(d->second);
                }
            } else if (!p.item.changed_dest().empty()) {
                fs::path placed;
                if (!PlaceUnder(*deps_.cas, merkle, o, p.item.changed_dest(), &err, &placed)) p.place_failed = true;
                o.set_placed(placed.string());
            }
            p.placed.push_back(std::move(o));
        }
    }

    std::lock_guard lock(mu_);
    for (auto& p : probes) {
        Item& it = run->items[p.i];
        it.in_lookup = false;
        it.looked_up = true;
        auto* c = it.result.mutable_cache();
        if (it.state != v1::ITEM_QUEUED) continue; // cancelled meanwhile
        if (p.hit && !p.place_failed) {
            c->set_hit(true);
            c->set_key(p.hit_key);
            c->set_source(p.source);
            c->set_reason(p.source.empty() ? "ran before on this team"
                                           : "from " + (deps_.hostname ? deps_.hostname(p.source) : p.source));
            for (auto& o : p.placed) *it.result.add_outputs() = std::move(o);
            *it.result.mutable_stdout() = p.hit->stdout();
            *it.result.mutable_stderr() = p.hit->stderr();
            for (const auto& f : p.hit->footprint()) it.result.add_footprint(f);
            Terminal(*run, p.i, v1::ITEM_SUCCEEDED, {});
        } else {
            c->set_hit(false);
            c->set_reason(p.place_failed ? "the cached result could not be placed" : p.reason);
            for (const auto& ch : p.changed) c->add_changed(ch);
            c->set_key(p.key);
        }
    }
    dirty_ = true;
}

// ---- batching ---------------------------------------------------------------------------------

void RunEngine::Dispatch(const RunPtr& run) {
    struct Launch {
        std::string id;
        v1::BatchSpec spec;
        std::set<std::string> exclude;
    };
    std::vector<Launch> launches;
    {
        std::lock_guard lock(mu_);
        if (run->cancelled || run->done) return;
        // Ready items, grouped by what a batch must share.
        struct Key {
            size_t stage;
            bool local_only;
            std::set<std::string> exclude;
            std::string root;
            auto operator<=>(const Key&) const = default;
        };
        std::map<Key, std::vector<size_t>> groups;
        for (size_t i = 0; i < run->items.size(); ++i) {
            const Item& it = run->items[i];
            if (it.state != v1::ITEM_QUEUED || !it.looked_up || it.in_lookup || !it.batch.empty()) continue;
            groups[Key{it.stage, it.local_only, it.exclude, it.root.hash}].push_back(i);
        }
        if (groups.empty()) return;

        auto seconds = [&](const Item& it) {
            const v1::Stage& st = run->stages[it.stage];
            if (auto s = deps_.resources->Seconds(st.kind(), it.spec.cost())) return std::max(*s, 0.01);
            return kDefaultItemSeconds;
        };
        auto memory = [&](const Item& it) -> int64_t {
            const v1::Stage& st = run->stages[it.stage];
            int64_t m = st.resources().mem_bytes();
            if (m <= 0) m = deps_.resources->Memory(st.kind(), it.spec.cost()).value_or(0);
            return std::max(m, it.mem_override);
        };

        int limit = 1;
        for (const auto& [key, members] : groups) {
            const v1::Stage& st = run->stages[key.stage];
            const int slots = deps_.slots ? deps_.slots(st.resources(), key.local_only ? v1::LOCAL_ONLY : st.where()) : 1;
            limit = std::max(limit, std::max(slots, 1) + std::max(1, slots / 2));
        }
        int in_flight = static_cast<int>(run->batches.size());

        for (auto& [key, members] : groups) {
            if (in_flight >= limit) break;
            const v1::Stage& st = run->stages[key.stage];
            const int slots = std::max(1, deps_.slots ? deps_.slots(st.resources(), st.where()) : 1);
            double total = 0;
            for (size_t i : members) total += seconds(run->items[i]);
            const double target = std::clamp(total / (2.0 * slots), kMinBatchSeconds, kMaxBatchSeconds);
            const int fixed = run->policy.batch();

            std::vector<std::vector<size_t>> chunks;
            if (!st.combine().argv().empty()) {
                // Consecutive items (by seq, same cwd) form one process.
                std::sort(members.begin(), members.end(), [&](size_t a, size_t b) {
                    return run->items[a].spec.seq() < run->items[b].spec.seq();
                });
                std::vector<size_t> cur;
                double cur_s = 0;
                for (size_t i : members) {
                    const Item& it = run->items[i];
                    const bool consecutive = !cur.empty() && it.spec.seq() == run->items[cur.back()].spec.seq() + 1 &&
                                             it.spec.cwd() == run->items[cur.back()].spec.cwd();
                    const bool full = fixed > 0 ? static_cast<int>(cur.size()) >= fixed : cur_s >= target;
                    if (!cur.empty() && (!consecutive || full || cur.size() >= kMaxItemsPerBatch)) {
                        chunks.push_back(std::move(cur));
                        cur.clear();
                        cur_s = 0;
                    }
                    cur.push_back(i);
                    cur_s += seconds(it);
                }
                if (!cur.empty()) chunks.push_back(std::move(cur));
            } else {
                // Longest first, so the slowest items do not end up in the tail.
                std::sort(members.begin(), members.end(), [&](size_t a, size_t b) {
                    const Item& x = run->items[a];
                    const Item& y = run->items[b];
                    if (x.spec.priority() != y.spec.priority()) return x.spec.priority() > y.spec.priority();
                    return seconds(x) > seconds(y);
                });
                std::vector<size_t> cur;
                double cur_s = 0;
                for (size_t i : members) {
                    cur.push_back(i);
                    cur_s += seconds(run->items[i]);
                    const bool full = fixed > 0 ? static_cast<int>(cur.size()) >= fixed : cur_s >= target;
                    if (full || cur.size() >= kMaxItemsPerBatch) {
                        chunks.push_back(std::move(cur));
                        cur.clear();
                        cur_s = 0;
                    }
                }
                if (!cur.empty()) chunks.push_back(std::move(cur));
            }

            for (auto& chunk : chunks) {
                if (in_flight >= limit) break;
                ++in_flight;
                const std::string bid = "b" + std::to_string(++run->batch_seq);
                v1::BatchSpec b;
                b.set_name(run->id + "/" + bid);
                *b.mutable_input_root() = node::ToProto(run->items[chunk.front()].root);
                *b.mutable_env() = st.env();
                *b.mutable_resources() = st.resources();
                b.mutable_resources()->set_cores(std::max(st.resources().cores(), 1));
                // Reserve what the items are expected to use; cap only what the adapter declared
                // (raised after an OOM kill). A learned figure is never a hard limit.
                int64_t mem = 0, limit = 0;
                for (size_t i : chunk) {
                    mem = std::max(mem, memory(run->items[i]));
                    if (st.resources().mem_bytes() > 0) {
                        limit = std::max({limit, st.resources().mem_bytes(), run->items[i].mem_override});
                    }
                }
                b.mutable_resources()->set_mem_bytes(mem);
                b.set_mem_limit(limit);
                b.set_kind(st.kind());
                b.set_warm(st.warm());
                b.set_network(st.network());
                b.set_timeout_s(run->policy.timeout_s());
                b.set_hung_after_s(run->policy.hung_after_s());
                b.set_preemptible(true);
                b.set_stream_logs(run->spec.stream_logs());
                b.set_requester(deps_.node_id);
                b.set_where(key.local_only ? v1::LOCAL_ONLY : st.where());
                if (!st.combine().argv().empty()) *b.mutable_combine() = st.combine();
                BatchRun br;
                br.cores = b.resources().cores();
                for (size_t i : chunk) {
                    Item& it = run->items[i];
                    it.batch = bid;
                    br.items.push_back(i);
                    auto* bi = b.add_items();
                    bi->set_id(it.spec.id());
                    *bi->mutable_argv() = it.spec.argv();
                    *bi->mutable_env() = it.spec.env();
                    bi->set_cwd(WorkspacePath("", it.spec.cwd()));
                    for (const auto& o : it.spec.outputs()) {
                        if (o.optional())
                            bi->add_optional_outputs(o.path());
                        else
                            bi->add_outputs(o.path());
                    }
                    bi->set_capture_changed(!it.spec.changed_dest().empty());
                    bi->set_depfile(it.spec.depfile());
                    bi->set_seq(it.spec.seq());
                    if (!it.checkpoint.hash().empty()) *bi->mutable_checkpoint() = it.checkpoint;
                }
                run->batches[bid] = std::move(br);
                launches.push_back(Launch{bid, std::move(b), key.exclude});
            }
        }
    }

    std::weak_ptr<Run> weak = run;
    const auto guard = guard_;
    // Runs `f` only while the engine is alive.
    auto guarded = [guard](auto f) {
        return [guard, f = std::move(f)](auto&&... args) {
            std::shared_lock alive(guard->mu);
            if (!guard->dead) f(std::forward<decltype(args)>(args)...);
        };
    };
    for (auto& l : launches) {
        const std::string bid = l.id;
        node::BatchCallbacks cb;
        cb.on_placed = guarded([this, weak, bid](const std::string& node) {
            auto r = weak.lock();
            if (!r) return;
            std::lock_guard lock(mu_);
            auto it = r->batches.find(bid);
            if (it == r->batches.end()) return;
            it->second.node = node;
            it->second.started_ms = NowMs();
            auto& lanes = r->busy_tids[node];
            size_t lane = 0;
            while (lane < lanes.size() && lanes[lane]) ++lane;
            if (lane == lanes.size()) lanes.push_back(false);
            lanes[lane] = true;
            it->second.tid = static_cast<int>(lane);
        });
        cb.on_started = guarded([this, weak, bid](const std::string& id) {
            auto r = weak.lock();
            if (!r) return;
            std::lock_guard lock(mu_);
            auto bi = r->batches.find(bid);
            auto ii = r->index.find(id);
            if (bi == r->batches.end() || ii == r->index.end()) return;
            Item& it = r->items[ii->second];
            if (IsTerminal(it.state) || it.batch != bid) return;
            it.node = bi->second.node;
            it.started_ms = NowMs();
            Update(*r, ii->second, v1::ITEM_RUNNING, it.node);
        });
        cb.on_log = guarded([this, weak](const v1::ItemLog& log) {
            auto r = weak.lock();
            if (!r) return;
            std::lock_guard lock(mu_);
            v1::RunEvent ev;
            *ev.mutable_log() = log;
            Emit(*r, std::move(ev), /*keep=*/false);
        });
        cb.on_item = guarded([this, weak, bid](const v1::ItemOutcome& o, const std::string& node) {
            if (auto r = weak.lock()) OnItem(r, bid, o, node);
        });
        cb.on_checkpoint = guarded([this, weak](const std::string& id, const v1::Digest& tree) {
            auto r = weak.lock();
            if (!r) return;
            std::lock_guard lock(mu_);
            if (auto ii = r->index.find(id); ii != r->index.end()) r->items[ii->second].checkpoint = tree;
        });
        cb.on_reject = guarded([this, weak](const std::string& node, v1::RejectKind kind, const std::string& reason) {
            if (kind == v1::REJECT_BUSY || kind == v1::REJECT_DRAINING) return; // passing
            auto r = weak.lock();
            if (!r) return;
            std::lock_guard lock(mu_);
            auto& share = r->nodes[node];
            if (share.excluded() == reason) return;
            share.set_node(node);
            if (deps_.hostname) share.set_hostname(deps_.hostname(node));
            share.set_excluded(reason);
            v1::RunEvent ev;
            ev.mutable_notice()->set_node(node);
            ev.mutable_notice()->set_text((share.hostname().empty() ? node.substr(0, 8) : share.hostname()) +
                                          " excluded: " + reason);
            Emit(*r, std::move(ev));
        });
        cb.on_done = guarded([this, weak, bid](node::BatchDone d) {
            if (auto r = weak.lock()) OnBatchDone(r, bid, std::move(d));
        });
        deps_.executor->Execute(run->id + "/" + bid, l.spec, std::move(l.exclude), std::move(cb));
    }
}

// ---- outcomes ---------------------------------------------------------------------------------

void RunEngine::OnItem(const RunPtr& run, const std::string& batch, const v1::ItemOutcome& o, const std::string& node) {
    v1::Item spec;
    v1::Stage stage;
    store::Digest root;
    std::string weak;
    bool use_cache = true;
    int64_t started = 0;
    size_t i = 0;
    {
        std::lock_guard lock(mu_);
        auto ii = run->index.find(o.id());
        if (ii == run->index.end()) return;
        i = ii->second;
        Item& it = run->items[i];
        if (IsTerminal(it.state) || it.batch != batch) return; // a hedge copy's late duplicate
        auto bi = run->batches.find(batch);
        if (bi != run->batches.end()) bi->second.reported.insert(i);
        spec = it.spec;
        stage = run->stages[it.stage];
        root = it.root;
        weak = it.weak;
        use_cache = run->policy.cache();
        started = it.started_ms > 0 ? it.started_ms : NowMs() - o.wall_ms();
    }

    v1::Attempt attempt;
    attempt.set_node(node);
    if (deps_.hostname) attempt.set_hostname(deps_.hostname(node));
    attempt.set_started_ms(started);
    attempt.set_wall_ms(o.wall_ms());
    attempt.set_cpu_ms(o.cpu_ms());
    attempt.set_peak_mem(o.peak_mem());
    attempt.set_batch(batch);
    int64_t out_bytes = 0;
    for (const auto& f : o.outputs()) out_bytes += std::max<int64_t>(f.digest().size_bytes(), 0);
    attempt.set_bytes_out(out_bytes);
    attempt.mutable_failure()->set_kind(o.failure());
    attempt.mutable_failure()->set_message(o.message());
    attempt.mutable_failure()->set_exit_code(o.exit_code());

    std::vector<v1::OutputFile> placed;
    std::string place_error;
    if (o.failure() == v1::FAILURE_NONE) {
        std::map<std::string, std::string> dest;
        for (const auto& d : spec.outputs()) dest[OutputKey(d.path())] = d.dest();
        for (auto f : o.outputs()) {
            std::string err;
            if (auto d = dest.find(OutputKey(f.path())); d != dest.end()) {
                if (!d->second.empty()) {
                    if (!Place(*deps_.cas, *deps_.merkle, f, d->second, &err) && place_error.empty()) place_error = err;
                    f.set_placed(d->second);
                }
            } else if (!spec.changed_dest().empty()) {
                fs::path p;
                if (!PlaceUnder(*deps_.cas, *deps_.merkle, f, spec.changed_dest(), &err, &p) && place_error.empty()) {
                    place_error = err;
                }
                f.set_placed(p.string());
            }
            placed.push_back(std::move(f));
        }
        if (place_error.empty()) {
            if (use_cache) {
                std::vector<std::string> footprint(o.footprint().begin(), o.footprint().end());
                const v1::Footprint observed = Observe(*deps_.merkle, root, spec, footprint);
                v1::CachedItem ci;
                for (const auto& f : o.outputs()) *ci.add_outputs() = f;
                *ci.mutable_stdout() = o.stdout();
                *ci.mutable_stderr() = o.stderr();
                ci.set_wall_ms(o.wall_ms());
                ci.set_peak_mem(o.peak_mem());
                ci.set_node(node);
                for (const auto& f : o.footprint()) ci.add_footprint(f);
                deps_.cache->Put(FullKey(weak, observed), ci);
                deps_.cache->AddFootprint(weak, observed);
            }
            deps_.resources->Record(stage.kind(), spec.cost(), o.wall_ms(), o.peak_mem());
        }
    }

    std::lock_guard lock(mu_);
    Item& it = run->items[i];
    if (IsTerminal(it.state) || it.batch != batch) return;
    run->core_s += static_cast<double>(o.wall_ms()) / 1000.0 * std::max(stage.resources().cores(), 1);
    auto& share = run->nodes[node];
    share.set_node(node);
    if (deps_.hostname) share.set_hostname(deps_.hostname(node));
    share.set_core_s(share.core_s() + static_cast<double>(o.wall_ms()) / 1000.0 * std::max(stage.resources().cores(), 1));
    if (auto bi = run->batches.find(batch); bi != run->batches.end()) {
        v1::TraceEvent t;
        t.set_name(o.id());
        t.set_ph("X");
        t.set_ts((started - run->started_ms) * 1000);
        t.set_dur(o.wall_ms() * 1000);
        t.set_pid(share.hostname().empty() ? node : share.hostname());
        t.set_tid(std::max(bi->second.tid, 0));
        run->timeline.push_back(std::move(t));
    }
    *it.result.add_attempts() = attempt;
    if (o.has_stdout()) *it.result.mutable_stdout() = o.stdout();
    if (o.has_stderr()) *it.result.mutable_stderr() = o.stderr();

    if (o.failure() == v1::FAILURE_NONE && place_error.empty()) {
        it.result.clear_outputs();
        for (auto& f : placed) *it.result.add_outputs() = std::move(f);
        it.result.clear_footprint();
        for (const auto& f : o.footprint()) it.result.add_footprint(f);
        it.checkpoint.Clear();
        share.set_items(share.items() + 1);
        Terminal(*run, i, v1::ITEM_SUCCEEDED, {});
    } else if (!place_error.empty()) {
        Fail(*run, i, v1::FAILURE_SANDBOX, "cannot place output: " + place_error);
    } else {
        switch (o.failure()) {
        case v1::FAILURE_EXIT:
            if (it.exit_failures < run->policy.exit_retries()) {
                ++it.exit_failures;
                Requeue(*run, i, "exit " + std::to_string(o.exit_code()) + ", retrying");
            } else {
                Fail(*run, i, v1::FAILURE_EXIT, o.message(), o.exit_code(), o.stdout_tail(), o.stderr_tail());
            }
            break;
        case v1::FAILURE_OOM:
            if (it.oom_retries < kMaxOomRetries) {
                ++it.oom_retries;
                int64_t had = std::max<int64_t>(stage.resources().mem_bytes(), it.mem_override);
                if (had <= 0) had = deps_.resources->Memory(stage.kind(), spec.cost()).value_or(256 * kMiB);
                deps_.resources->RaiseMemory(stage.kind(), spec.cost(), had);
                it.mem_override = std::max<int64_t>(had + had / 2, 512 * kMiB);
                Requeue(*run, i, "out of memory, retrying with " + std::to_string(it.mem_override / kMiB) + " MiB");
            } else {
                Fail(*run, i, v1::FAILURE_OOM, o.message(), o.exit_code(), o.stdout_tail(), o.stderr_tail());
            }
            break;
        case v1::FAILURE_SANDBOX:
            ++it.infra_failures;
            it.exclude.insert(node);
            if (it.infra_failures <= run->policy.infra_retries()) {
                Requeue(*run, i, "sandbox failed on " + share.hostname() + ", retrying elsewhere");
            } else {
                Fail(*run, i, v1::FAILURE_SANDBOX, o.message(), o.exit_code(), o.stdout_tail(), o.stderr_tail());
            }
            break;
        default:
            Fail(*run, i, o.failure(), o.message(), o.exit_code(), o.stdout_tail(), o.stderr_tail());
            break;
        }
    }
    dirty_ = true;
    cv_.notify_all();
}

void RunEngine::OnBatchDone(const RunPtr& run, const std::string& batch, node::BatchDone d) {
    std::lock_guard lock(mu_);
    auto bi = run->batches.find(batch);
    if (bi == run->batches.end()) return;
    BatchRun br = std::move(bi->second);
    run->batches.erase(bi);
    if (br.tid >= 0) {
        auto& lanes = run->busy_tids[br.node];
        if (static_cast<size_t>(br.tid) < lanes.size()) lanes[static_cast<size_t>(br.tid)] = false;
    }
    run->pushed += d.bytes_pushed;
    run->pulled += d.bytes_pulled;
    run->seeded += d.bytes_seeded;

    const std::string where = d.node.empty() ? std::string("a node")
                                             : (deps_.hostname ? deps_.hostname(d.node) : d.node.substr(0, 8));
    for (size_t i : br.items) {
        Item& it = run->items[i];
        if (IsTerminal(it.state) || it.batch != batch) continue;
        it.batch.clear();
        if (run->cancelled) {
            v1::Failure f;
            f.set_kind(v1::FAILURE_CANCELLED);
            f.set_message("run ended: " + run->ended_by);
            Terminal(*run, i, v1::ITEM_CANCELLED, std::move(f));
            continue;
        }
        const bool unplaceable = d.failure == v1::FAILURE_ENV_UNAVAILABLE || d.failure == v1::FAILURE_TOOLCHAIN_MISMATCH;
        const bool can_fallback = run->policy.local_fallback() && !it.fallback_tried && !it.local_only &&
                                  run->stages[it.stage].where() != v1::REMOTE_ONLY;
        if (d.failure == v1::FAILURE_CANCELLED) {
            Requeue(*run, i, "its batch was stopped");
        } else if (unplaceable) {
            if (can_fallback) {
                it.fallback_tried = true;
                it.local_only = true;
                it.exclude.clear();
                Requeue(*run, i, d.message + "; trying this node");
            } else {
                Fail(*run, i, d.failure, d.message);
            }
        } else {
            // The donor left, the sandbox failed, or the batch ended without this item.
            ++it.infra_failures;
            if (!d.node.empty() && d.node != deps_.node_id) it.exclude.insert(d.node);
            const std::string why = d.failure == v1::FAILURE_NONE ? "the batch ended without it" : d.message;
            if (it.infra_failures <= run->policy.infra_retries()) {
                Requeue(*run, i,
                        "retry " + std::to_string(it.infra_failures) + "/" + std::to_string(run->policy.infra_retries()) +
                            ": " + why);
            } else if (can_fallback) {
                it.fallback_tried = true;
                it.local_only = true;
                it.exclude.clear();
                Requeue(*run, i, why + "; trying this node");
            } else {
                Fail(*run, i, d.failure == v1::FAILURE_NONE ? v1::FAILURE_DONOR_LEFT : d.failure,
                     why + " (on " + where + ")");
            }
        }
    }
    dirty_ = true;
    cv_.notify_all();
}

// ---- the end ----------------------------------------------------------------------------------

void RunEngine::Finish(const RunPtr& run) {
    v1::Report report;
    {
        std::lock_guard lock(mu_);
        if (run->done) return;
        run->done = true;
        run->finished_ms = NowMs();
        run->finished_at = std::chrono::steady_clock::now();
        report = ReportLocked(*run);
        v1::RunEvent ev;
        *ev.mutable_finished() = report.run();
        Emit(*run, std::move(ev));
        for (auto& w : run->watchers) w->End();
        run->watchers.clear();
        // Keep a few finished runs in memory; the rest are served from the history.
        size_t finished = 0;
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            auto r = runs_.find(*it);
            if (r != runs_.end() && r->second->done && ++finished > kFinishedRunsKept) r->second.reset();
        }
        std::erase_if(order_, [&](const std::string& id) {
            auto r = runs_.find(id);
            if (r == runs_.end() || r->second) return false;
            runs_.erase(r);
            return true;
        });
    }
    WriteReport(run, report);
}

void RunEngine::WriteReport(const RunPtr& run, const v1::Report& report) {
    const std::string json = ToJson(report);
    WriteFileAtomically(deps_.runs_dir / (run->id + ".json"), json);
    const std::string& path = run->spec.report_path();
    if (path.empty()) return;
    WriteFileAtomically(path, json);
    // The timeline alone, in Chrome trace format, loads in Perfetto.
    std::ostringstream trace;
    trace << "{\"traceEvents\":[";
    for (int i = 0; i < report.timeline_size(); ++i) {
        const auto& t = report.timeline(i);
        std::string name = t.name(), pid = t.pid();
        for (auto* s : {&name, &pid}) {
            std::string esc;
            for (char c : *s) {
                if (c == '"' || c == '\\') esc.push_back('\\');
                esc.push_back(c);
            }
            *s = esc;
        }
        trace << (i ? "," : "") << "{\"name\":\"" << name << "\",\"ph\":\"X\",\"ts\":" << t.ts() << ",\"dur\":" << t.dur()
              << ",\"pid\":\"" << pid << "\",\"tid\":" << t.tid() << "}";
    }
    trace << "]}";
    fs::path tp = path;
    tp.replace_extension(".trace.json");
    WriteFileAtomically(tp, trace.str());
}

// ---- reading ----------------------------------------------------------------------------------

v1::RunStatus RunEngine::StatusLocked(const Run& run) const {
    v1::RunStatus st;
    st.set_run_id(run.id);
    st.set_name(run.spec.name());
    st.set_done(run.done);
    st.set_open(run.open);
    st.set_total(static_cast<int32_t>(run.items.size()));
    for (const auto& it : run.items) {
        switch (it.state) {
        case v1::ITEM_SUCCEEDED:
            st.set_succeeded(st.succeeded() + 1);
            if (it.result.cache().hit()) st.set_cached(st.cached() + 1);
            break;
        case v1::ITEM_FAILED:
            st.set_failed(st.failed() + 1);
            break;
        case v1::ITEM_CANCELLED:
            st.set_cancelled(st.cancelled() + 1);
            break;
        case v1::ITEM_RUNNING:
            st.set_running(st.running() + 1);
            break;
        default:
            st.set_queued(st.queued() + 1);
            break;
        }
    }
    st.set_started_ms(run.started_ms);
    st.set_finished_ms(run.finished_ms);
    st.set_bytes_pushed(static_cast<int64_t>(run.pushed));
    st.set_bytes_pulled(static_cast<int64_t>(run.pulled));
    st.set_bytes_seeded(static_cast<int64_t>(run.seeded));
    st.set_core_s(run.core_s);
    for (const auto& [node, share] : run.nodes) *st.add_nodes() = share;
    st.set_report_path(run.spec.report_path());
    st.set_workspace_path(run.spec.workspace_path());
    st.set_ended_by(run.ended_by);
    return st;
}

v1::Report RunEngine::ReportLocked(const Run& run) const {
    v1::Report r;
    *r.mutable_run() = StatusLocked(run);
    for (const auto& it : run.items) *r.add_items() = it.result;
    for (const auto& t : run.timeline) *r.add_timeline() = t;
    return r;
}

std::optional<v1::RunStatus> RunEngine::Status(const std::string& run_id) const {
    {
        std::lock_guard lock(mu_);
        auto it = runs_.find(run_id);
        if (it != runs_.end() && it->second) return StatusLocked(*it->second);
    }
    if (auto rep = Report(run_id)) return rep->run();
    return std::nullopt;
}

std::optional<v1::Report> RunEngine::Report(const std::string& run_id) const {
    {
        std::lock_guard lock(mu_);
        auto it = runs_.find(run_id);
        if (it != runs_.end() && it->second) return ReportLocked(*it->second);
    }
    if (run_id.empty() || run_id.find_first_of("/.") != std::string::npos) return std::nullopt;
    std::ifstream in(deps_.runs_dir / (run_id + ".json"));
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    v1::Report rep;
    google::protobuf::util::JsonParseOptions opts;
    opts.ignore_unknown_fields = true;
    if (!google::protobuf::util::JsonStringToMessage(ss.str(), &rep, opts).ok()) return std::nullopt;
    return rep;
}

std::shared_ptr<RunEvents> RunEngine::Watch(const std::string& run_id, bool from_start) {
    std::lock_guard lock(mu_);
    auto it = runs_.find(run_id);
    if (it == runs_.end() || !it->second) {
        // A run only in the history: its final status, then the end.
        auto stream = std::make_shared<RunEvents>();
        std::ifstream in(deps_.runs_dir / (run_id + ".json"));
        if (!in || run_id.find_first_of("/.") != std::string::npos) return nullptr;
        if (auto rep = [&]() -> std::optional<v1::Report> {
                std::stringstream ss;
                ss << in.rdbuf();
                v1::Report r;
                google::protobuf::util::JsonParseOptions o;
                o.ignore_unknown_fields = true;
                if (!google::protobuf::util::JsonStringToMessage(ss.str(), &r, o).ok()) return std::nullopt;
                return r;
            }()) {
            v1::RunEvent ev;
            ev.set_run_id(run_id);
            *ev.mutable_finished() = rep->run();
            stream->Push(ev);
        }
        stream->End();
        return stream;
    }
    Run& run = *it->second;
    auto stream = std::make_shared<RunEvents>();
    if (from_start) {
        for (const auto& ev : run.history) stream->Push(ev);
    } else if (run.done && !run.history.empty()) {
        stream->Push(run.history.back());
    }
    if (run.done)
        stream->End();
    else
        run.watchers.push_back(stream);
    return stream;
}

std::vector<v1::RunStatus> RunEngine::List() const {
    std::vector<v1::RunStatus> out;
    std::set<std::string> seen;
    {
        std::lock_guard lock(mu_);
        for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
            auto r = runs_.find(*it);
            if (r == runs_.end() || !r->second) continue;
            out.push_back(StatusLocked(*r->second));
            seen.insert(*it);
        }
    }
    std::stable_partition(out.begin(), out.end(), [](const v1::RunStatus& s) { return !s.done(); });
    std::vector<std::pair<fs::file_time_type, std::string>> files;
    std::error_code ec;
    for (fs::directory_iterator it(deps_.runs_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        const std::string id = it->path().stem().string();
        if (!seen.contains(id)) files.emplace_back(it->last_write_time(ec), id);
    }
    std::sort(files.rbegin(), files.rend());
    for (const auto& [t, id] : files) {
        if (auto rep = Report(id)) out.push_back(rep->run());
    }
    return out;
}

std::unordered_set<std::string> RunEngine::ProtectedBlobHashes() const {
    std::set<std::string> roots;
    std::vector<v1::OutputFile> outputs;
    {
        std::lock_guard lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        for (const auto& [id, run] : runs_) {
            if (!run) continue;
            if (run->done && now - run->finished_at > kOutputGrace) continue;
            if (!run->done) {
                if (!run->spec.workspace().hash().empty()) roots.insert(run->spec.workspace().hash());
                for (const auto& st : run->stages) {
                    if (st.env().has_tree()) roots.insert(st.env().tree().hash());
                }
            }
            for (const auto& it : run->items) {
                if (!run->done && !it.root.hash.empty()) roots.insert(it.root.hash);
                if (!it.checkpoint.hash().empty()) roots.insert(it.checkpoint.hash());
                for (const auto& o : it.result.outputs()) outputs.push_back(o);
            }
        }
    }
    std::unordered_set<std::string> out;
    for (const auto& h : roots) {
        try {
            for (const auto& d : deps_.merkle->CollectTree({h, 0})) out.insert(d.hash);
        } catch (const std::exception&) {
            out.insert(h);
        }
    }
    for (const auto& o : outputs) {
        if (o.symlink()) continue;
        if (!o.is_dir()) {
            out.insert(o.digest().hash());
            continue;
        }
        try {
            for (const auto& d : deps_.merkle->CollectTree(node::FromProto(o.digest()))) out.insert(d.hash);
        } catch (const std::exception&) {
            out.insert(o.digest().hash());
        }
    }
    return out;
}

} // namespace zeromesh::run
