#include "sched/placer.hpp"

#include "node/convert.hpp"
#include "sched/dispatch_policy.hpp"
#include "sched/rank.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zeromesh::sched {

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

int MaxRetries(const v1::TaskSpec& spec) {
    if (!spec.policy().has_max_retries()) return 3;
    return std::max(0, spec.policy().max_retries());
}

// Only donor-drain preemption is retryable on a finished TaskResult. Run timeouts,
// non-zero exits, and donor env/policy errors are final (peer loss / push / pull are
// retried before a TaskResult exists).
bool IsRetryable(const v1::TaskResult& r, bool drain_preempted, bool user_cancelled) {
    if (user_cancelled) return false;
    if (r.timed_out()) return false;
    if (r.cancelled()) return drain_preempted;
    return false;
}

std::string RetryReason(const v1::TaskResult& r, bool drain_preempted) {
    if (drain_preempted) return "donor drained";
    if (!r.error().empty()) return r.error();
    return "retry";
}

bool OfferRejectionRetryable(const std::string& reason) {
    return reason.find("in use") != std::string::npos || reason.find("draining") != std::string::npos;
}

// Capability miss on one donor: try the next peer, and offer this one again after
// kCapabilityRecheck (a donor may install the command while the job runs).
bool OfferRejectionTryNextPeer(const std::string& reason) {
    return reason.find("not found on this node") != std::string::npos ||
           reason.find("no env provider") != std::string::npos;
}

constexpr std::chrono::seconds kCapabilityRecheck{15};

} // namespace

struct Placer::Task {
    std::string id;
    std::string job_id;
    v1::DispatchPolicy dispatch;
    v1::TaskSpec spec;
    node::TaskCallbacks cb;
    std::chrono::steady_clock::time_point enqueued{std::chrono::steady_clock::now()};
    std::string place_on;  // node the dispatcher reserved for the next attempt

    // Placement state across attempts. Only the dispatcher (while queued) or the one
    // worker running the task touches these, never both at once.
    int attempt{0};                         // failed attempts that consumed a retry
    std::unordered_set<std::string> skip;   // nodes that failed this task (infra)
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lacks_until;  // command missing
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> busy_until;   // donor refused: full
    std::string last_reason{"no eligible nodes"};
    std::optional<v1::TaskResult> last_attempt_result;  // last real run, if any
    std::optional<std::vector<v1::Digest>> needed;
    int64_t input_tree_bytes{-1};

    std::atomic<bool> cancelled{false};       // user/job cancel: final
    std::atomic<bool> drain_preempted{false};  // donor drain: retryable cancel
    std::atomic<bool> finished{false};
    bool reserved{false};  // place_on's capacity is held; cleared on release

    std::mutex mu;
    bool is_local{false};
    std::string assigned_node;  // node currently reserved / running
    std::shared_ptr<node::NodeClient> client;

    bool IsLocal() {
        std::lock_guard lock(mu);
        return is_local;
    }

    void Done(v1::TaskResult r) {
        if (finished.exchange(true)) return;
        result_node = r.executed_by();
        result_ok = r.exit_code() == 0 && r.error().empty() && !r.cancelled() && !r.timed_out();
        result_ms = r.duration_ms();
        if (cb.on_done) cb.on_done(std::move(r));
    }
    std::string result_node;
    bool result_ok{false};
    int64_t result_ms{0};
};

struct Placer::Shared {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<Task>> tasks;
    std::map<std::string, int> in_flight;  // node id -> tasks we run there

    int InFlightLocked(const std::string& node) const {
        auto it = in_flight.find(node);
        return it == in_flight.end() ? 0 : it->second;
    }

    void AddLocked(const std::string& node, int delta) {
        if ((in_flight[node] += delta) <= 0) in_flight.erase(node);
    }

    void Forget(const std::string& task_id) {
        std::lock_guard lock(mu);
        tasks.erase(task_id);
    }
};

Placer::Placer(std::shared_ptr<node::LocalExecutor> local, std::shared_ptr<net::PeerTable> peers,
               ClientFactory factory, std::shared_ptr<store::CasStore> cas,
               std::shared_ptr<store::MerkleBuilder> merkle, std::shared_ptr<plugin::Registry> registry,
               std::string local_node_id, std::function<net::Telemetry()> telemetry)
    : local_(std::move(local)),
      peers_(std::move(peers)),
      factory_(std::move(factory)),
      cas_(std::move(cas)),
      merkle_(std::move(merkle)),
      registry_(std::move(registry)),
      local_id_(std::move(local_node_id)),
      telemetry_(std::move(telemetry)),
      shared_(std::make_shared<Shared>()) {
    if (!peers_ || !factory_ || !cas_ || !merkle_ || !registry_) {
        throw std::invalid_argument("Placer: missing dependency");
    }
    peers_->RegisterPreemptCallback([this](const std::string& node_id, const std::string& reason) {
        OnPeerDrain(node_id, reason);
    });
    dispatcher_ = std::jthread([this](std::stop_token st) { DispatcherLoop(st); });
}

int64_t Placer::InputTreeBytes(const v1::TaskSpec& spec) const {
    const auto root = node::FromProto(spec.input_root());
    if (root.hash.empty()) return 0;
    int64_t sum = 0;
    for (const auto& d : merkle_->CollectTree(root)) {
        sum += std::max<int64_t>(d.size_bytes, 0);
    }
    return sum;
}

namespace {

std::string PushCacheKey(const std::string& node_id, const std::string& input_root_hash) {
    return node_id + '\0' + input_root_hash;
}

} // namespace

int64_t Placer::EstimatedMissingBytes(const std::string& node_id, const std::string& input_root_hash,
                                      int64_t total_bytes) const {
    if (total_bytes <= 0) return 0;
    std::lock_guard lock(push_cache_mu_);
    auto it = push_cache_.find(PushCacheKey(node_id, input_root_hash));
    if (it == push_cache_.end()) return -1;
    return std::max<int64_t>(0, total_bytes - it->second);
}

void Placer::NoteInputsPushed(const std::string& node_id, const std::string& input_root_hash, int64_t bytes_pushed,
                              int64_t total_bytes) {
    if (total_bytes <= 0) return;
    const std::string key = PushCacheKey(node_id, input_root_hash);
    std::lock_guard lock(push_cache_mu_);
    int64_t prev = 0;
    if (auto it = push_cache_.find(key); it != push_cache_.end()) prev = it->second;
    const int64_t on_peer =
        std::min(total_bytes, prev + std::max<int64_t>(0, bytes_pushed));
    push_cache_[key] = on_peer;
    push_cache_lru_.remove(key);
    push_cache_lru_.push_front(key);
    while (push_cache_.size() > kPushCacheMax) {
        const std::string evict = push_cache_lru_.back();
        push_cache_lru_.pop_back();
        push_cache_.erase(evict);
    }
}

Placer::~Placer() {
    dispatcher_.request_stop();
    dispatch_cv_.notify_all();
    dispatcher_ = std::jthread{};
    std::vector<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard lock(shared_->mu);
        for (auto& [id, t] : shared_->tasks) tasks.push_back(t);
    }
    for (auto& t : tasks) {
        if (!t->IsLocal()) Cancel(t->id);
    }
    {
        std::lock_guard lock(workers_mu_);
        workers_.clear();
    }
    // Workers are gone; anything still queued completes once, as cancelled.
    std::map<std::string, JobQueue> left;
    {
        std::lock_guard lock(dispatch_mu_);
        left.swap(jobs_);
    }
    for (auto& [jid, job] : left) {
        for (auto& t : job.pending) t->Done(Cancelled(local_id_));
    }
}

void Placer::WakeDispatcher() {
    {
        std::lock_guard lock(dispatch_mu_);
        dispatch_dirty_ = true;
    }
    dispatch_cv_.notify_all();
}

std::vector<std::string> Placer::NodesWithInFlight() const {
    std::lock_guard lock(shared_->mu);
    std::vector<std::string> out;
    out.reserve(shared_->in_flight.size());
    for (const auto& [id, n] : shared_->in_flight) {
        if (n > 0 && id != local_id_) out.push_back(id);
    }
    return out;
}

int Placer::InFlightOn(const std::string& node_id) const {
    std::lock_guard lock(shared_->mu);
    return shared_->InFlightLocked(node_id);
}

void Placer::ReapFinishedLocked() {
    std::erase_if(workers_, [](const Worker& w) { return w.done->load(); });
}

void Placer::OnPeerDrain(const std::string& node_id, const std::string& /*reason*/) {
    std::vector<std::shared_ptr<Task>> hit;
    {
        std::lock_guard lock(shared_->mu);
        for (auto& [id, t] : shared_->tasks) {
            std::lock_guard tlock(t->mu);
            if (t->assigned_node == node_id && !t->is_local) hit.push_back(t);
        }
    }
    for (auto& t : hit) {
        t->drain_preempted = true;
        std::shared_ptr<node::NodeClient> client;
        {
            std::lock_guard lock(t->mu);
            client = t->client;
        }
        if (client) client->Cancel(t->id);
    }
}

void Placer::Execute(const std::string& task_id, const v1::TaskSpec& spec, node::TaskCallbacks cb,
                     node::ExecuteContext ctx) {
    auto task = std::make_shared<Task>();
    task->id = task_id;
    task->job_id = std::move(ctx.job_id);
    task->dispatch = std::move(ctx.dispatch);
    if (task->job_id.empty()) task->job_id = task_id;  // a lone task is its own job
    task->spec = spec;
    const net::Telemetry self = telemetry_ ? telemetry_() : net::Telemetry{};
    if (task->spec.resources().arch().empty() && !self.arch.empty()) {
        task->spec.mutable_resources()->set_arch(self.arch);
    }
    task->input_tree_bytes = InputTreeBytes(task->spec);
    task->cb = std::move(cb);
    {
        std::lock_guard lock(shared_->mu);
        shared_->tasks[task_id] = task;
    }
    {
        std::lock_guard lock(dispatch_mu_);
        auto [it, fresh] = jobs_.try_emplace(task->job_id);
        if (fresh) {
            it->second.job_id = task->job_id;
            it->second.dispatch = task->dispatch;
        }
        it->second.pending.push_back(task);
        dispatch_dirty_ = true;
    }
    dispatch_cv_.notify_all();
}

void Placer::Requeue(const std::shared_ptr<Task>& task) {
    {
        std::lock_guard lock(dispatch_mu_);
        task->place_on.clear();
        jobs_[task->job_id].pending.push_front(task);
        dispatch_dirty_ = true;
    }
    dispatch_cv_.notify_all();
}

void Placer::Cancel(const std::string& task_id) {
    std::shared_ptr<Task> task;
    {
        std::lock_guard lock(shared_->mu);
        auto it = shared_->tasks.find(task_id);
        if (it == shared_->tasks.end()) return;
        task = it->second;
    }
    task->cancelled = true;
    bool is_local = false;
    std::shared_ptr<node::NodeClient> client;
    {
        std::lock_guard lock(task->mu);
        is_local = task->is_local;
        client = task->client;
    }
    if (is_local) {
        if (local_) local_->Cancel(task_id);
    } else if (client) {
        client->Cancel(task_id);
    }
}

v1::TaskResult Placer::RunLocal(const std::shared_ptr<Task>& task, const v1::TaskSpec& spec) {
    {
        std::lock_guard lock(task->mu);
        task->is_local = true;
        task->assigned_node = local_id_;
    }
    std::promise<v1::TaskResult> done;
    auto fut = done.get_future();
    node::TaskCallbacks cb;
    cb.on_state = task->cb.on_state;
    cb.on_log = task->cb.on_log;
    cb.on_done = [&done](v1::TaskResult r) { done.set_value(std::move(r)); };
    local_->Execute(task->id, spec, std::move(cb));
    if (task->cancelled) local_->Cancel(task->id);
    v1::TaskResult r = fut.get();
    {
        std::lock_guard lock(task->mu);
        task->is_local = false;
        task->assigned_node.clear();
    }
    return r;
}

namespace {

bool PullOutputs(node::NodeClient& client, store::CasStore& cas, const v1::TaskResult& r, std::string* error) {
    std::unordered_set<std::string> asked;
    std::vector<v1::Digest> blobs;
    std::vector<v1::Digest> trees;
    auto want = [&](const v1::Digest& d, std::vector<v1::Digest>& into) {
        if (d.hash().empty() || !asked.insert(d.hash()).second) return;
        into.push_back(d);
    };
    want(r.stdout(), blobs);
    want(r.stderr(), blobs);
    for (const auto& o : r.outputs()) {
        if (o.is_symlink()) continue;
        want(o.digest(), o.is_dir() ? trees : blobs);
    }

    auto pull_missing = [&](const std::vector<v1::Digest>& ds) {
        std::vector<v1::Digest> missing;
        for (const auto& d : ds) {
            if (!cas.HasBlob(d.hash())) missing.push_back(d);
        }
        return client.Pull(cas, missing, error);
    };
    if (!pull_missing(blobs)) return false;
    while (!trees.empty()) {
        if (!pull_missing(trees)) return false;
        std::vector<v1::Digest> files;
        std::vector<v1::Digest> next;
        for (const auto& t : trees) {
            auto data = cas.GetBlob(t.hash());
            auto m = data ? store::DirectoryManifest::Deserialize(*data) : std::nullopt;
            if (!m) {
                if (error) *error = "output tree " + t.hash() + " is corrupt";
                return false;
            }
            for (const auto& f : m->files) want(node::ToProto(f.digest), files);
            for (const auto& d : m->directories) want(node::ToProto(d.digest), next);
        }
        if (!pull_missing(files)) return false;
        trees = std::move(next);
    }
    return true;
}

} // namespace

namespace {

constexpr std::chrono::milliseconds kDispatchTick{250};  // peer changes and backoff expiry
constexpr std::chrono::seconds kBusyBackoff{1};          // a donor refused: its slots are full

} // namespace

bool Placer::RunPlacement(const std::shared_ptr<Task>& task) {
    const v1::TaskSpec& spec = task->spec;
    const int max_retries = MaxRetries(spec);
    const std::string node_id = task->place_on;
    const bool is_local = node_id == local_id_;
    const std::string& input_root_hash = spec.input_root().hash();
    const int64_t input_tree_bytes = std::max<int64_t>(task->input_tree_bytes, 0);

    auto release = [&] {
        std::lock_guard lock(shared_->mu);
        if (task->reserved) shared_->AddLocked(node_id, -1);
        task->reserved = false;
        std::lock_guard tlock(task->mu);
        if (task->assigned_node == node_id) task->assigned_node.clear();
        task->client.reset();
        task->is_local = false;
    };
    // Charges a retry and sends the task back for another node; false when none are left.
    auto retry = [&](const std::string& reason) {
        if (task->attempt >= max_retries) return false;
        ++task->attempt;
        task->skip.insert(node_id);
        task->drain_preempted = false;  // fresh attempt
        if (task->cb.on_state) {
            task->cb.on_state(v1::QUEUED, "retry " + std::to_string(task->attempt) + "/" +
                                              std::to_string(max_retries) + ": " + reason);
        }
        Requeue(task);
        return true;
    };
    auto fail_placement = [&](const std::string& reason) {
        if (task->last_attempt_result) {
            task->Done(std::move(*task->last_attempt_result));
        } else {
            task->Done(Failed(local_id_, "no node can run this task: " + reason));
        }
        return false;
    };
    auto infra_retry_or_fail = [&](const std::string& reason) {
        task->last_reason = reason;
        return retry(reason) || fail_placement(reason);
    };
    // Donor drain may arrive as MarkDraining, or only as a cancelled TaskResult if the
    // preempt callback raced behind CancelPreemptible. Treat PeerState::Draining as drain.
    auto donor_draining = [&] {
        if (task->drain_preempted.load()) return true;
        if (auto p = peers_->Get(node_id); p && p->state == net::PeerState::Draining) {
            task->drain_preempted = true;
            return true;
        }
        return false;
    };
    auto finish = [&](v1::TaskResult r) {
        task->Done(std::move(r));
        return false;
    };

    // User/job cancel always wins over drain preemption.
    if (task->cancelled.load()) {
        release();
        return finish(Cancelled(local_id_));
    }
    {
        std::lock_guard lock(task->mu);
        task->assigned_node = node_id;
        task->is_local = is_local;
    }

    if (is_local) {
        if (!local_) {
            release();
            return infra_retry_or_fail("local executor unavailable");
        }
        auto result = RunLocal(task, spec);
        release();
        task->last_attempt_result = result;
        const bool user_cancel = task->cancelled.load();
        const bool drain = donor_draining();
        if (user_cancel) return finish(Cancelled(local_id_));
        if (IsRetryable(result, drain, user_cancel) && retry(RetryReason(result, drain))) return true;
        return finish(std::move(result));
    }

    auto peer = peers_->Get(node_id);
    auto client = peer ? factory_(*peer) : nullptr;
    if (!client) {
        release();
        return infra_retry_or_fail("node " + node_id + ": not reachable");
    }
    if (!task->needed) {
        task->needed.emplace();
        std::unordered_set<std::string> seen;
        auto add_digest = [&](const store::Digest& d) {
            if (d.hash.empty() || !seen.insert(d.hash).second) return;
            task->needed->push_back(node::ToProto(d));
        };
        for (const auto& d : merkle_->CollectTree(node::FromProto(spec.input_root()))) add_digest(d);
        if (spec.env_spec().name() == "mirror") {
            auto it = spec.env_spec().params().find("root_digest");
            if (it != spec.env_spec().params().end() && store::CasStore::IsValidHash(it->second)) {
                store::Digest root;
                root.hash = it->second;
                if (auto bytes = cas_->GetBlob(it->second)) root.size_bytes = static_cast<int64_t>(bytes->size());
                add_digest(root);
            }
        }
    }
    {
        std::lock_guard lock(task->mu);
        task->client = client;
    }

    std::string err;
    if (node::EncodedOfferByteSize(task->id, spec, *task->needed) >
        static_cast<size_t>(node::kNodeApiMaxMessageSize)) {
        release();
        return fail_placement("input tree too large to offer: " + std::to_string(task->needed->size()) +
                              " digests");
    }
    auto reply = client->Offer(task->id, spec, *task->needed, &err);
    if (!reply) {
        release();
        return infra_retry_or_fail("node " + node_id + ": " + err);
    }
    if (!reply->accepted()) {
        release();
        const std::string why = reply->reason();
        if (OfferRejectionRetryable(why)) {
            task->busy_until[node_id] = std::chrono::steady_clock::now() + kBusyBackoff;
            Requeue(task);
            return true;
        }
        if (OfferRejectionTryNextPeer(why)) {
            task->lacks_until[node_id] = std::chrono::steady_clock::now() + kCapabilityRecheck;
            task->last_reason = "node " + node_id + ": " + why;
            Requeue(task);
            return true;
        }
        return finish(Failed(local_id_, "no node can run this task: node " + node_id + ": " + why));
    }
    if (task->cb.on_state) task->cb.on_state(v1::PLACED, "node " + node_id);

    std::vector<v1::Digest> missing(reply->missing().begin(), reply->missing().end());
    if (!client->Push(*cas_, missing, &err)) {
        release();
        return infra_retry_or_fail("node " + node_id + ": uploading inputs failed: " + err);
    }
    {
        int64_t pushed = 0;
        for (const auto& d : missing) pushed += std::max<int64_t>(d.size_bytes(), 0);
        if (missing.empty()) pushed = input_tree_bytes;
        NoteInputsPushed(node_id, input_root_hash, pushed, input_tree_bytes);
    }
    if (task->cancelled.load()) {
        release();
        return finish(Cancelled(local_id_));
    }

    bool cancel_forwarded = false;
    auto on_event = [&](const v1::JobEvent& ev) {
        if ((task->cancelled || task->drain_preempted) && !cancel_forwarded) {
            cancel_forwarded = true;
            client->Cancel(task->id);
        }
        if (ev.has_result()) return;
        if (ev.log_fd() != 0) {
            if (task->cb.on_log) task->cb.on_log(ev.log_fd(), ev.log_chunk());
        } else if ((ev.state() == v1::PLACED || ev.state() == v1::RUNNING) && task->cb.on_state) {
            task->cb.on_state(ev.state(), ev.detail());
        }
    };
    auto result = client->Run(task->id, on_event, &err);
    const bool user_cancel = task->cancelled.load();
    const bool drain = donor_draining();

    if (!result) {
        release();
        if (user_cancel) return finish(Cancelled(node_id));
        v1::TaskResult lost = Failed(node_id, "node " + node_id + " lost during run (" + err + ")");
        task->last_attempt_result = lost;
        if (retry(lost.error())) return true;
        return finish(std::move(lost));
    }
    result->set_executed_by(node_id);
    task->last_attempt_result = *result;

    // Decide retry before pulling outputs so partial donor outputs stay discarded.
    if (IsRetryable(*result, drain, user_cancel) && task->attempt < max_retries) {
        release();
        return retry(RetryReason(*result, drain));
    }
    if (user_cancel || result->cancelled()) {
        release();
        return finish(Cancelled(node_id));
    }
    if (!PullOutputs(*client, *cas_, *result, &err)) {
        release();
        v1::TaskResult lost = Failed(node_id, "node " + node_id + " lost while sending outputs: " + err);
        task->last_attempt_result = lost;
        if (retry(lost.error())) return true;
        return finish(std::move(lost));
    }
    release();
    return finish(std::move(*result));
}

void Placer::StartWorker(const std::shared_ptr<Task>& task) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard lock(workers_mu_);
    ReapFinishedLocked();
    workers_.push_back(Worker{done, std::jthread([this, task, done] {
                                  bool requeued = false;
                                  try {
                                      requeued = RunPlacement(task);
                                  } catch (const std::exception& e) {
                                      {
                                          std::lock_guard slock(shared_->mu);
                                          if (task->reserved) shared_->AddLocked(task->place_on, -1);
                                          task->reserved = false;
                                      }
                                      task->Done(Failed(local_id_, std::string("placer: ") + e.what()));
                                  }
                                  {
                                      std::lock_guard dlock(dispatch_mu_);
                                      if (auto it = jobs_.find(task->job_id); it != jobs_.end()) {
                                          if (it->second.in_flight > 0) --it->second.in_flight;
                                      }
                                      // Serialized with Decide, which also runs under dispatch_mu_.
                                      if (!requeued && task->finished.load()) {
                                          if (auto* p = LookupDispatchPolicy(*registry_, task->dispatch.kind())) {
                                              p->OnResult(task->id, task->result_node, task->result_ok,
                                                          task->result_ms);
                                          }
                                      }
                                      dispatch_dirty_ = true;
                                  }
                                  if (!requeued && task->finished.load()) shared_->Forget(task->id);
                                  dispatch_cv_.notify_all();
                                  done->store(true);
                              })});
}

void Placer::DispatcherLoop(std::stop_token st) {
    while (!st.stop_requested()) {
        {
            std::unique_lock lock(dispatch_mu_);
            dispatch_cv_.wait_for(lock, st, kDispatchTick, [&] { return dispatch_dirty_; });
            if (st.stop_requested()) break;
            dispatch_dirty_ = false;
        }
        DispatchOnce();
    }
}

void Placer::DispatchOnce() {
    const auto now = std::chrono::steady_clock::now();
    const net::Telemetry self = telemetry_ ? telemetry_() : net::Telemetry{};
    std::vector<net::Peer> peers;
    for (auto& p : peers_->ListActive()) {
        if (p.id != local_id_) peers.push_back(std::move(p));
    }

    std::vector<plugin::DispatchNodeView> nodes;
    {
        std::lock_guard lock(shared_->mu);
        if (local_) {
            nodes.push_back(plugin::DispatchNodeView{.node_id = local_id_,
                                                     .is_local = true,
                                                     .slots = self.slots,
                                                     .queue_depth = self.queue_depth,
                                                     .in_flight = shared_->InFlightLocked(local_id_),
                                                     .cores_total = self.cores_total,
                                                     .mem_available = self.mem_available,
                                                     .cpu_load_pct = self.cpu_load_pct,
                                                     .on_ac = self.on_ac,
                                                     .latency_ms = 0,
                                                     .missing_bytes = 0,
                                                     .arch = self.arch});
        }
        for (const auto& p : peers) {
            nodes.push_back(plugin::DispatchNodeView{.node_id = p.id,
                                                     .is_local = false,
                                                     .slots = p.slots,
                                                     .queue_depth = p.queue_depth,
                                                     .in_flight = shared_->InFlightLocked(p.id),
                                                     .cores_total = p.cores,
                                                     .mem_available = p.mem_available,
                                                     .cpu_load_pct = p.cpu_load_pct,
                                                     .on_ac = p.is_charging,
                                                     .latency_ms = static_cast<int64_t>(p.latency.count()),
                                                     .missing_bytes = -1,
                                                     .arch = p.arch});
        }
    }

    auto active = [&](const std::unordered_map<std::string, std::chrono::steady_clock::time_point>& until,
                      const std::string& id) {
        auto it = until.find(id);
        return it != until.end() && now < it->second;
    };

    std::vector<std::pair<std::shared_ptr<Task>, v1::TaskResult>> finished;
    std::vector<std::shared_ptr<Task>> to_start;
    {
        std::lock_guard lock(dispatch_mu_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            JobQueue& job = it->second;
            const std::string& kind = job.dispatch.kind();
            plugin::IDispatchPolicy* policy = LookupDispatchPolicy(*registry_, kind);

            std::vector<plugin::DispatchTaskView> views;
            std::deque<std::shared_ptr<Task>> keep;
            for (auto& t : job.pending) {
                if (t->cancelled.load()) {
                    finished.emplace_back(t, Cancelled(local_id_));
                    continue;
                }
                if (!policy) {
                    finished.emplace_back(t, Failed(local_id_, "unknown dispatch policy '" + kind + "'"));
                    continue;
                }
                plugin::DispatchTaskView v;
                v.id = t->id;
                v.name = t->spec.name();
                v.cores = std::max(t->spec.resources().cores(), 1);
                v.mem_bytes = t->spec.resources().mem_bytes();
                v.attempt = t->attempt;
                v.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t->enqueued).count();
                v.preemptible = !t->spec.policy().has_preemptible() || t->spec.policy().preemptible();
                v.may_run_local = !t->spec.policy().has_may_run_local() || t->spec.policy().may_run_local();
                v.may_run_remote = !t->spec.policy().has_may_run_remote() || t->spec.policy().may_run_remote();
                v.arch = t->spec.resources().arch();
                v.input_bytes = std::max<int64_t>(t->input_tree_bytes, 0);
                // Failed here, or lacks the command: may make the task unplaceable.
                for (const auto& n : nodes) {
                    if (t->skip.contains(n.node_id) || active(t->lacks_until, n.node_id)) {
                        v.exclude_nodes.push_back(n.node_id);
                    }
                }
                bool placeable = false;
                for (const auto& n : nodes) {
                    plugin::DispatchNodeView idle = n;
                    idle.in_flight = 0;
                    if (Fits(v, n) && Capacity(idle, v.cores, 0) >= 1) {
                        placeable = true;
                        break;
                    }
                }
                if (!placeable) {
                    if (t->last_attempt_result) {
                        finished.emplace_back(t, *t->last_attempt_result);
                    } else {
                        finished.emplace_back(t, Failed(local_id_, "no node can run this task: " + t->last_reason));
                    }
                    continue;
                }
                // Recently refused for being full: wait, but this does not make the task unplaceable.
                for (const auto& n : nodes) {
                    if (active(t->busy_until, n.node_id)) v.exclude_nodes.push_back(n.node_id);
                }
                views.push_back(std::move(v));
                keep.push_back(t);
            }
            job.pending = std::move(keep);
            if (job.pending.empty()) {
                it = job.in_flight == 0 ? jobs_.erase(it) : std::next(it);
                continue;
            }

            std::vector<plugin::DispatchNodeView> job_nodes = nodes;
            const Task& head = *job.pending.front();
            for (auto& n : job_nodes) {
                if (!n.is_local) {
                    n.missing_bytes = EstimatedMissingBytes(n.node_id, head.spec.input_root().hash(),
                                                            std::max<int64_t>(head.input_tree_bytes, 0));
                }
            }
            const std::map<std::string, std::string> params(job.dispatch.params().begin(),
                                                            job.dispatch.params().end());
            const int max_in_flight = ParamInt(params, "max_in_flight", 0);
            const auto assigns = policy->Decide(views, job_nodes, params, job.in_flight);

            std::unordered_map<std::string, size_t> view_of;
            for (size_t i = 0; i < views.size(); ++i) view_of[views[i].id] = i;
            for (const auto& a : assigns) {
                if (max_in_flight > 0 && job.in_flight >= max_in_flight) break;
                auto vi = view_of.find(a.task_id);
                if (vi == view_of.end()) continue;  // unknown or already assigned
                const plugin::DispatchTaskView& v = views[vi->second];
                auto node = std::find_if(job_nodes.begin(), job_nodes.end(),
                                         [&](const plugin::DispatchNodeView& n) { return n.node_id == a.node_id; });
                if (node == job_nodes.end() || !Fits(v, *node)) continue;
                if (Capacity(*node, v.cores, node->queue_depth) < 1) continue;  // past the donor's limit
                auto pit = std::find_if(job.pending.begin(), job.pending.end(),
                                        [&](const std::shared_ptr<Task>& t) { return t->id == a.task_id; });
                if (pit == job.pending.end()) continue;
                std::shared_ptr<Task> task = *pit;
                job.pending.erase(pit);
                view_of.erase(vi);

                ++node->in_flight;
                for (auto& n : nodes) {
                    if (n.node_id == a.node_id) ++n.in_flight;  // later jobs this round see it too
                }
                {
                    std::lock_guard slock(shared_->mu);
                    shared_->AddLocked(a.node_id, 1);
                }
                task->reserved = true;
                task->place_on = a.node_id;
                ++job.in_flight;
                to_start.push_back(std::move(task));
            }
            ++it;
        }
    }
    for (auto& [t, r] : finished) {
        t->Done(std::move(r));
        shared_->Forget(t->id);
    }
    for (auto& t : to_start) StartWorker(t);
}

} // namespace zeromesh::sched
