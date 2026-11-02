#include "node/local_executor.hpp"

#include "node/convert.hpp"
#include "node/env_check.hpp"
#include "node/requester_blob_source.hpp"

#include <algorithm>
#include <exception>
#include <fstream>

namespace zeromesh::node {

namespace {

constexpr std::chrono::milliseconds kMemoryPoll{100};
// Free memory left after a batch starts. A process's footprint grows over its first seconds, so a
// burst of starts that each fit the free memory *now* can still add up to more than the node has.
// The headroom is a share of RAM (earlyoom-style), within a floor and a ceiling.
constexpr uint64_t kMemoryHeadroomFloor = 128ull << 20;
constexpr uint64_t kMemoryHeadroomCeil = 768ull << 20;
constexpr double kMemoryHeadroomShare = 0.15;
constexpr std::chrono::milliseconds kStartSpacing{50};

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

BatchDone Done(const std::string& node, v1::FailureKind kind, std::string message, bool cancelled = false) {
    BatchDone d;
    d.node = node;
    d.failure = kind;
    d.message = std::move(message);
    d.cancelled = cancelled;
    return d;
}

} // namespace

LocalExecutor::LocalExecutor(std::shared_ptr<exec::BatchRunner> runner, std::shared_ptr<plugin::Registry> registry,
                             std::string node_id, int core_capacity, std::filesystem::path state_dir,
                             std::shared_ptr<store::CasStore> cas, NodeClientFactory client_factory)
    : runner_(std::move(runner)), registry_(std::move(registry)), node_id_(std::move(node_id)),
      core_capacity_(std::max(1, core_capacity)), state_dir_(std::move(state_dir)), cas_(std::move(cas)),
      client_factory_(std::move(client_factory)) {
    workers_.reserve(static_cast<size_t>(core_capacity_));
    for (int i = 0; i < core_capacity_; ++i) {
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

    std::deque<std::shared_ptr<Item>> left;
    {
        std::lock_guard lock(mu_);
        left.swap(queue_);
        items_.clear();
    }
    for (auto& item : left) {
        if (item->cb.on_done) item->cb.on_done(Done(node_id_, v1::FAILURE_CANCELLED, "node shutting down", true));
    }
}

void LocalExecutor::Execute(const std::string& batch_id, const v1::BatchSpec& spec, std::set<std::string>,
                            BatchCallbacks cb) {
    auto item = std::make_shared<Item>();
    item->batch_id = batch_id;
    item->spec = spec;
    item->cb = std::move(cb);
    item->cores = std::clamp(spec.resources().cores(), 1, core_capacity_);
    item->mem = std::max<int64_t>(spec.resources().mem_bytes(), 0);
    {
        std::lock_guard lock(mu_);
        items_[batch_id] = item;
        queue_.push_back(item);
    }
    cv_.notify_all();
}

void LocalExecutor::Cancel(const std::string& batch_id) {
    std::shared_ptr<Item> dequeued;
    {
        std::lock_guard lock(mu_);
        auto it = items_.find(batch_id);
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
    if (dequeued->cb.on_done) dequeued->cb.on_done(Done(node_id_, v1::FAILURE_CANCELLED, "cancelled", true));
}

int LocalExecutor::CoresInUse() const {
    std::lock_guard lock(mu_);
    return cores_in_use_;
}

int64_t LocalExecutor::MemInUse() const {
    std::lock_guard lock(mu_);
    return mem_in_use_;
}

std::vector<LocalExecutor::Active> LocalExecutor::ListActive() const {
    std::lock_guard lock(mu_);
    std::vector<Active> out;
    for (const auto& [id, item] : items_) out.push_back({item->batch_id, item->spec, item->started, item->done, item->current});
    return out;
}

// A batch that declares memory starts only when that much is free (plus headroom), and no sooner
// than kStartSpacing after the previous start so free memory can catch up. The first batch always
// runs, so an undersized node still makes progress.
bool LocalExecutor::MemoryAllowsLocked(const Item& item) const {
    if (item.mem <= 0 || mem_in_use_ == 0) return true;
    if (std::chrono::steady_clock::now() - last_start_ < kStartSpacing) return false;
    const MemInfo m = ReadMemInfo();
    if (m.available == UINT64_MAX) return true;
    const uint64_t headroom = std::clamp(static_cast<uint64_t>(static_cast<double>(m.total) * kMemoryHeadroomShare),
                                         kMemoryHeadroomFloor, kMemoryHeadroomCeil);
    return m.available >= static_cast<uint64_t>(item.mem) + headroom;
}

void LocalExecutor::Worker(std::stop_token st) {
    for (;;) {
        std::shared_ptr<Item> item;
        {
            std::unique_lock lock(mu_);
            // Polled, not only notified: free memory changes without anyone calling notify.
            while (!cv_.wait_for(lock, st, kMemoryPoll, [this] {
                return !queue_.empty() && cores_in_use_ + queue_.front()->cores <= core_capacity_ &&
                       MemoryAllowsLocked(*queue_.front());
            })) {
                if (st.stop_requested()) return;
            }
            item = queue_.front();
            queue_.pop_front();
            cores_in_use_ += item->cores;
            mem_in_use_ += item->mem;
            last_start_ = std::chrono::steady_clock::now();
            item->started = true;
        }
        BatchDone done = Run(*item);
        {
            std::lock_guard lock(mu_);
            cores_in_use_ -= item->cores;
            mem_in_use_ -= item->mem;
            auto it = items_.find(item->batch_id);
            if (it != items_.end() && it->second == item) items_.erase(it);
        }
        cv_.notify_all();
        if (item->cb.on_done) item->cb.on_done(std::move(done));
    }
}

BatchDone LocalExecutor::Run(Item& item) {
    const v1::BatchSpec& spec = item.spec;
    if (item.stop.stop_requested()) return Done(node_id_, v1::FAILURE_CANCELLED, "cancelled", true);
    if (item.cb.on_placed) item.cb.on_placed(node_id_);

    const bool tree = spec.env().has_tree();
    plugin::IEnvProvider* env = registry_ ? registry_->Env(tree ? "tree" : "host") : nullptr;
    if (env == nullptr) {
        return Done(node_id_, v1::FAILURE_ENV_UNAVAILABLE, std::string("no ") + (tree ? "tree" : "host") +
                                                               " environment provider on this node");
    }

    plugin::BatchContext ctx;
    ctx.batch_id = item.batch_id;
    if (tree) {
        ctx.env.params[plugin::kTreeParam] = spec.env().tree().hash();
        ctx.env.params[plugin::kRequesterParam] = spec.requester();
    }
    ctx.requester_node_id = spec.requester().empty() ? node_id_ : spec.requester();
    std::error_code ec;
    const auto scratch = state_dir_ / "scratch" / ("env-" + store::CasStore::ComputeDigest(item.batch_id).hash);
    std::filesystem::create_directories(scratch, ec);
    ctx.scratch_dir = scratch;
    ctx.cache_dir = state_dir_ / "envs";
    if (cas_) ctx.profile_dir = cas_->GetBasePath() / "profiles";

    std::unique_ptr<plugin::IBlobSource> blob_source;
    if (tree) {
        if (ctx.requester_node_id == node_id_) {
            blob_source = std::make_unique<LocalCasBlobSource>(cas_);
        } else {
            auto client = client_factory_ ? client_factory_(ctx.requester_node_id) : nullptr;
            if (!client) {
                return Done(node_id_, v1::FAILURE_ENV_UNAVAILABLE,
                            "requester " + ctx.requester_node_id + " is not reachable to fetch the environment");
            }
            blob_source = std::make_unique<RequesterBlobSource>(cas_, std::move(client));
        }
        ctx.blob_source = blob_source.get();
    }

    try {
        if (auto avail = env->Check(ctx.env); !avail.ok) return Done(node_id_, v1::FAILURE_ENV_UNAVAILABLE, avail.reason);
        auto prepared = env->Prepare(ctx);
        if (!prepared.value) {
            std::filesystem::remove_all(scratch, ec);
            return Done(node_id_, v1::FAILURE_ENV_UNAVAILABLE,
                        prepared.error.empty() ? "the environment could not be prepared" : prepared.error);
        }

        exec::BatchRunSpec rs;
        rs.input_root = FromProto(spec.input_root());
        rs.sandbox_env = std::move(*prepared.value);
        std::map<std::string, std::string> vars(spec.env().vars().begin(), spec.env().vars().end());
        rs.env.assign(vars.begin(), vars.end());
        for (const auto& bi : spec.items()) {
            exec::ItemSpec it;
            it.id = bi.id();
            it.argv.assign(bi.argv().begin(), bi.argv().end());
            std::map<std::string, std::string> ienv(bi.env().begin(), bi.env().end());
            it.env.assign(ienv.begin(), ienv.end());
            it.cwd = bi.cwd().empty() ? "." : bi.cwd();
            it.outputs.assign(bi.outputs().begin(), bi.outputs().end());
            it.optional_outputs.assign(bi.optional_outputs().begin(), bi.optional_outputs().end());
            it.capture_changed = bi.capture_changed();
            it.depfile = bi.depfile();
            it.seq = bi.seq();
            it.checkpoint = FromProto(bi.checkpoint());
            rs.items.push_back(std::move(it));
        }
        rs.combine_argv.assign(spec.combine().argv().begin(), spec.combine().argv().end());
        rs.cores = std::max(spec.resources().cores(), 1);
        rs.mem_bytes = static_cast<uint64_t>(std::max<int64_t>(spec.mem_limit(), 0));
        rs.network = spec.network();
        rs.timeout = std::chrono::seconds(spec.timeout_s() > 0 ? spec.timeout_s() : 3600);
        rs.hung_after = std::chrono::seconds(std::max(spec.hung_after_s(), 0));
        if (spec.warm()) {
            rs.warm_dir = state_dir_ / "warm" /
                          store::CasStore::ComputeDigest(spec.kind() + "\n" + EnvIdentity(spec.env())).hash.substr(0, 32);
        }
        rs.node_id = node_id_;
        rs.tag = item.batch_id;
        rs.stream_logs = spec.stream_logs();

        exec::BatchCallbacks cb;
        cb.on_started = [this, &item](const std::string& id) {
            {
                std::lock_guard lock(mu_); // ListActive reads `current`
                item.current = id;
            }
            if (item.cb.on_started) item.cb.on_started(id);
        };
        cb.on_log = [&item](const std::string& id, int fd, std::string_view chunk) {
            if (!item.cb.on_log) return;
            v1::ItemLog log;
            log.set_id(id);
            log.set_fd(fd);
            log.set_chunk(std::string(chunk));
            item.cb.on_log(log);
        };
        cb.on_item = [this, &item](exec::ItemRunResult r) {
            {
                std::lock_guard lock(mu_);
                ++item.done;
            }
            if (item.cb.on_item) item.cb.on_item(ToProto(r), node_id_);
        };
        cb.on_checkpoint = [&item](const std::string& id, const store::Digest& tree) {
            if (item.cb.on_checkpoint) item.cb.on_checkpoint(id, ToProto(tree));
        };

        exec::BatchRunResult rr;
        try {
            rr = runner_->Run(rs, cb, item.stop.get_token());
        } catch (const std::exception& e) {
            env->Release(ctx);
            std::filesystem::remove_all(scratch, ec);
            return Done(node_id_, v1::FAILURE_SANDBOX, std::string("sandbox: ") + e.what());
        }
        env->Release(ctx);
        std::filesystem::remove_all(scratch, ec);
        if (!rr.error.empty()) return Done(node_id_, v1::FAILURE_SANDBOX, rr.error);
        if (rr.cancelled) return Done(node_id_, v1::FAILURE_CANCELLED, "cancelled", true);
        return Done(node_id_, v1::FAILURE_NONE, "");
    } catch (const std::exception& e) {
        std::filesystem::remove_all(scratch, ec);
        return Done(node_id_, v1::FAILURE_ENV_UNAVAILABLE, std::string("environment: ") + e.what());
    }
}

} // namespace zeromesh::node
