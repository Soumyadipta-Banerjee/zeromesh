#include <fstream>
#include "node/local_executor.hpp"

#include "node/convert.hpp"
#include "node/mirror_manifest.hpp"
#include "node/requester_blob_source.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>

namespace zeromesh::node {

namespace {

v1::TaskResult Failed(const std::string& node_id, std::string error) {
    v1::TaskResult r;
    r.set_exit_code(-1);
    r.set_executed_by(node_id);
    r.set_error(std::move(error));
    return r;
}

v1::TaskResult Cancelled(const std::string& node_id) {
    v1::TaskResult r = Failed(node_id, "cancelled");
    r.set_cancelled(true);
    return r;
}

v1::TaskResult ToProto(const exec::RunResult& rr, const std::string& node_id) {
    v1::TaskResult r;
    r.set_exit_code(rr.exit_code);
    r.set_timed_out(rr.timed_out);
    r.set_cancelled(rr.cancelled);
    *r.mutable_stdout() = node::ToProto(rr.stdout_digest);
    *r.mutable_stderr() = node::ToProto(rr.stderr_digest);
    r.set_stdout_head(rr.stdout_head);
    r.set_stderr_head(rr.stderr_head);
    for (const auto& o : rr.outputs) {
        auto* e = r.add_outputs();
        e->set_path(o.path);
        *e->mutable_digest() = node::ToProto(o.digest);
        e->set_is_dir(o.is_dir);
        e->set_is_executable(o.is_executable);
        e->set_is_symlink(o.is_symlink);
        e->set_symlink_target(o.symlink_target);
    }
    r.set_executed_by(node_id);
    r.set_duration_ms(rr.duration.count());
    r.set_error(rr.error);
    return r;
}

v1::TaskState StateFromResult(const v1::TaskResult& r) {
    if (r.cancelled()) return v1::CANCELLED;
    if (r.timed_out() || r.exit_code() != 0 || !r.error().empty()) return v1::FAILED;
    return v1::SUCCEEDED;
}

} // namespace

LocalExecutor::LocalExecutor(std::shared_ptr<exec::SandboxRunner> runner, std::shared_ptr<plugin::Registry> registry,
                             std::string node_id, int slots, std::filesystem::path scratch_root,
                             std::shared_ptr<store::CasStore> cas, NodeClientFactory client_factory)
    : runner_(std::move(runner)), registry_(std::move(registry)), node_id_(std::move(node_id)),
      scratch_root_(std::move(scratch_root)), cas_(std::move(cas)), client_factory_(std::move(client_factory)) {
    core_capacity_ = std::max(1, slots);
    int n = core_capacity_;
    workers_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this](std::stop_token st) { Worker(st); });
    }
}

LocalExecutor::~LocalExecutor() {
    {
        std::lock_guard lock(mu_);
        for (auto& [id, item] : items_) item->stop.request_stop();
    }
    for (auto& w : workers_) w.request_stop();
    cv_.notify_all();
    workers_.clear(); // joins

    // Tasks that never started still get their single on_done.
    std::deque<std::shared_ptr<Item>> left;
    {
        std::lock_guard lock(mu_);
        left.swap(queue_);
        items_.clear();
    }
    for (auto& item : left) {
        if (item->cb.on_done) item->cb.on_done(Cancelled(node_id_));
    }
}

void LocalExecutor::Execute(const std::string& task_id, const v1::TaskSpec& spec, TaskCallbacks cb) {
    auto item = std::make_shared<Item>();
    item->task_id = task_id;
    item->spec = spec;
    item->cb = std::move(cb);
    {
        std::lock_guard lock(mu_);
        items_[task_id] = item;
        queue_.push_back(item);
    }
    cv_.notify_one();
}

void LocalExecutor::Cancel(const std::string& task_id) {
    std::shared_ptr<Item> dequeued;
    {
        std::lock_guard lock(mu_);
        auto it = items_.find(task_id);
        if (it == items_.end()) return;
        auto item = it->second;
        if (item->started) {
            item->stop.request_stop();
            return;
        }
        std::erase(queue_, item);
        items_.erase(it);
        dequeued = std::move(item);
    }
    RememberFinished(dequeued->task_id, dequeued->spec, v1::CANCELLED);
    if (dequeued->cb.on_done) dequeued->cb.on_done(Cancelled(node_id_));
}

void LocalExecutor::RememberFinished(std::string task_id, v1::TaskSpec spec, v1::TaskState state) {
    std::lock_guard lock(mu_);
    recent_.push_front(ActiveItem{std::move(task_id), std::move(spec), true, state});
    while (recent_.size() > kRecentLimit) recent_.pop_back();
}

std::vector<LocalExecutor::ActiveItem> LocalExecutor::ListActive() const {
    std::lock_guard lock(mu_);
    std::vector<ActiveItem> out;
    out.reserve(items_.size() + recent_.size());
    for (const auto& [id, item] : items_) {
        out.push_back(ActiveItem{item->task_id, item->spec, item->started, item->started ? v1::RUNNING : v1::QUEUED});
    }
    for (const auto& done : recent_) out.push_back(done);
    return out;
}

void LocalExecutor::ClearFinished() {
    std::lock_guard lock(mu_);
    recent_.clear();
}

int LocalExecutor::CoresInUse() const {
    std::lock_guard lock(mu_);
    return cores_in_use_;
}

namespace {

constexpr std::chrono::milliseconds kMemoryPoll{100};
// Free memory left after a task starts. A compiler's footprint grows over its first seconds, so a
// burst of starts that each fit the free memory *now* can still add up to more than the node has.
// The floor is a fraction of total memory because oom killers such as earlyoom act when free memory
// falls below a share of it (10% by default); staying above that line is the point of the gate.
constexpr uint64_t kMemoryHeadroomFloor = 768ull << 20;
constexpr double kMemoryHeadroomShare = 0.15;
constexpr std::chrono::milliseconds kStartSpacing{150};

struct MemInfo {
    uint64_t total{UINT64_MAX};
    uint64_t available{UINT64_MAX};
};

MemInfo ReadMemInfo() {
    std::ifstream in("/proc/meminfo");
    std::string key, unit;
    uint64_t kb = 0;
    MemInfo m;
    while (in >> key >> kb >> unit) {
        if (key == "MemTotal:") m.total = kb * 1024;
        if (key == "MemAvailable:") m.available = kb * 1024;
    }
    return m; // unreadable fields stay at UINT64_MAX: do not hold work back
}

} // namespace

// A task that declares memory starts only when that much is free (plus headroom), and no sooner
// than kStartSpacing after the previous start so the node's free-memory figure can catch up.
// The first task always runs, so an undersized node still makes progress.
bool LocalExecutor::MemoryAllowsLocked(const Item& item) const {
    const int64_t need = item.spec.resources().mem_bytes();
    if (need <= 0 || mem_in_use_ == 0) return true;
    if (std::chrono::steady_clock::now() - last_start_ < kStartSpacing) return false;
    const MemInfo m = ReadMemInfo();
    if (m.available == UINT64_MAX) return true;
    const auto headroom = std::max<uint64_t>(kMemoryHeadroomFloor, static_cast<uint64_t>(m.total * kMemoryHeadroomShare));
    return m.available >= static_cast<uint64_t>(need) + headroom;
}

int64_t LocalExecutor::MemInUse() const {
    std::lock_guard lock(mu_);
    return mem_in_use_;
}

void LocalExecutor::Worker(std::stop_token st) {
    for (;;) {
        std::shared_ptr<Item> item;
        {
            std::unique_lock lock(mu_);
            // Polled, not only notified: free memory changes without anyone calling notify.
            while (!cv_.wait_for(lock, st, kMemoryPoll, [this] {
                if (queue_.empty()) return false;
                int need = std::max(queue_.front()->spec.resources().cores(), 1);
                if (need > core_capacity_) need = core_capacity_;
                return cores_in_use_ + need <= core_capacity_ && MemoryAllowsLocked(*queue_.front());
            })) {
                if (st.stop_requested()) return;
            }
            item = queue_.front();
            queue_.pop_front();
            int need = std::max(item->spec.resources().cores(), 1);
            if (need > core_capacity_) need = core_capacity_;
            item->cores_held = need;
            cores_in_use_ += need;
            item->mem_held = std::max<int64_t>(item->spec.resources().mem_bytes(), 0);
            mem_in_use_ += item->mem_held;
            last_start_ = std::chrono::steady_clock::now();
            item->started = true;
        }
        v1::TaskResult result = Run(*item);
        const v1::TaskState state = StateFromResult(result);
        {
            std::lock_guard lock(mu_);
            cores_in_use_ -= item->cores_held;
            mem_in_use_ -= item->mem_held;
            auto it = items_.find(item->task_id);
            if (it != items_.end() && it->second == item) items_.erase(it);
        }
        cv_.notify_all();
        RememberFinished(item->task_id, item->spec, state);
        if (item->cb.on_done) item->cb.on_done(std::move(result));
    }
}

v1::TaskResult LocalExecutor::Run(Item& item) {
    const v1::TaskSpec& spec = item.spec;
    if (item.stop.stop_requested()) return Cancelled(node_id_);
    if (item.cb.on_state) item.cb.on_state(v1::RUNNING, "");

    const std::string& env_name = spec.env_spec().name();
    plugin::IEnvProvider* env = registry_ ? registry_->Env(env_name) : nullptr;
    if (env == nullptr) return Failed(node_id_, "no env provider '" + env_name + "'");

    plugin::TaskContext ctx;
    ctx.task_id = item.task_id;
    ctx.env.params.insert(spec.env_spec().params().begin(), spec.env_spec().params().end());
    ctx.env.argv.assign(spec.argv().begin(), spec.argv().end());

    std::string requester;
    if (auto it = ctx.env.params.find(kMirrorRequesterParam); it != ctx.env.params.end()) {
        requester = it->second;
    }
    ctx.requester_node_id = requester.empty() ? node_id_ : requester;

    std::error_code ec;
    const auto scratch = scratch_root_ / store::CasStore::ComputeDigest(item.task_id).hash;
    std::filesystem::create_directories(scratch, ec);
    ctx.scratch_dir = scratch;

    std::unique_ptr<plugin::IBlobSource> blob_source;
    if (env_name == "mirror") {
        if (cas_) ctx.profile_dir = cas_->GetBasePath() / "profiles";
        if (requester.empty() || ctx.requester_node_id == node_id_) {
            blob_source = std::make_unique<LocalCasBlobSource>(cas_);
        } else {
            if (!client_factory_) {
                return Failed(node_id_, "mirror env: no peer client factory configured");
            }
            auto client = client_factory_(ctx.requester_node_id);
            if (!client) {
                return Failed(node_id_, "mirror env: requester " + ctx.requester_node_id + " is not reachable");
            }
            blob_source = std::make_unique<RequesterBlobSource>(cas_, std::move(client));
        }
        ctx.blob_source = blob_source.get();
    }

    try {
        plugin::Availability avail = env->Check(ctx.env);
        if (!avail.ok) {
            return Failed(node_id_,
                          avail.reason.empty() ? "env provider '" + env_name + "' cannot run this task" : avail.reason);
        }
        plugin::Result<plugin::SandboxEnv> prepared = env->Prepare(ctx);
        if (!prepared.value) {
            return Failed(node_id_, prepared.error.empty() ? "env provider '" + env_name + "' failed to prepare"
                                                           : prepared.error);
        }

        exec::RunSpec rs;
        rs.argv.assign(spec.argv().begin(), spec.argv().end());
        rs.env.assign(spec.env().begin(), spec.env().end());
        std::sort(rs.env.begin(), rs.env.end());
        rs.input_root = FromProto(spec.input_root());
        rs.working_dir = spec.working_dir().empty() ? "." : spec.working_dir();
        rs.outputs.assign(spec.outputs().begin(), spec.outputs().end());
        rs.capture_changed = spec.capture_changed();
        int timeout_s = spec.policy().timeout_s() > 0 ? spec.policy().timeout_s() : 3600;
        rs.timeout = std::chrono::seconds(timeout_s);
        rs.allow_network = spec.policy().allow_network();
        rs.sandbox_env = std::move(*prepared.value);
        rs.node_id = node_id_;
        rs.task_tag = item.task_id;
        if (item.cb.on_unit) {
            rs.on_unit = [&item](const std::string& unit_id, const store::Digest& digest) {
                item.cb.on_unit(unit_id, ToProto(digest));
            };
        }
        rs.cores = spec.resources().cores();
        if (spec.resources().mem_bytes() > 0) {
            rs.mem_bytes = static_cast<uint64_t>(spec.resources().mem_bytes());
        }

        exec::LogSink sink;
        if (item.cb.on_log) sink = [&item](int fd, std::string_view chunk) { item.cb.on_log(fd, chunk); };

        exec::RunResult rr;
        try {
            rr = runner_->Run(rs, sink, item.stop.get_token());
        } catch (const std::exception& e) {
            env->Release(ctx);
            return Failed(node_id_, std::string("sandbox: ") + e.what());
        }
        env->Release(ctx);
        std::filesystem::remove_all(scratch, ec);
        return ToProto(rr, node_id_);
    } catch (const std::exception& e) {
        std::filesystem::remove_all(scratch, ec);
        return Failed(node_id_, std::string("env provider '") + env_name + "': " + e.what());
    }
}

} // namespace zeromesh::node
