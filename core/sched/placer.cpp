#include "sched/placer.hpp"

#include "node/convert.hpp"
#include "sched/rank.hpp"

#include <algorithm>
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

// The donor's owner is busy and the comfort governor is holding donated work back; send
// elsewhere and look again soon.
bool OfferRejectionOwnerBusy(const std::string& reason) {
    return reason.find("owner busy") != std::string::npos;
}
constexpr std::chrono::seconds kOwnerBusyRecheck{5};

// Longest task horizon (seconds) used when ranking donors by departure hazard.
constexpr double kHazardHorizonCapS = 900.0;

} // namespace

struct Placer::Task {
    std::string id;
    v1::TaskSpec spec;
    node::TaskCallbacks cb;
    std::atomic<bool> cancelled{false};       // user/job cancel: final
    std::atomic<bool> drain_preempted{false}; // donor drain: retryable cancel
    std::atomic<bool> finished{false};
    std::atomic<bool> worker_done{false};

    // Tail hedging. A hedge is an extra copy of `primary`, registered under its own id; it never
    // reports to the job, only (on success) to the primary through cb.on_done.
    bool is_hedge{false};
    std::unordered_set<std::string> exclude; // nodes this task must not be placed on (hedge copies)
    std::atomic<bool> hedge_counted{false};  // counted in HedgeStats::launched once it got a slot

    std::mutex mu;
    bool is_local{false};
    std::string assigned_node; // node currently reserved / running
    std::shared_ptr<node::NodeClient> client;
    bool running{false};  // a remote Run is in progress
    bool ever_ran{false}; // a remote Run was started at least once
    std::chrono::steady_clock::time_point run_started{};
    std::vector<std::shared_ptr<Task>> hedges; // primary only
    int hedge_seq{0};
    std::chrono::steady_clock::time_point next_hedge{};
    std::function<void()> on_finished; // runs once after the task's result was delivered

    bool IsLocal() {
        std::lock_guard lock(mu);
        return is_local;
    }

    // True when this call delivered the result (the first one wins; later ones are ignored).
    bool Finish(v1::TaskResult r) {
        if (finished.exchange(true)) return false;
        std::function<void()> hook;
        {
            std::lock_guard lock(mu);
            hook = std::move(on_finished);
            on_finished = nullptr;
        }
        if (cb.on_done) cb.on_done(std::move(r));
        if (hook) hook();
        return true;
    }

    void Done(v1::TaskResult r) { Finish(std::move(r)); }
};

struct Placer::Shared {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<Task>> tasks;
    std::map<std::string, int> in_flight; // node id -> tasks we run there

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

Placer::Placer(std::shared_ptr<node::LocalExecutor> local, std::shared_ptr<net::PeerTable> peers, ClientFactory factory,
               std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
               std::string local_node_id, std::function<net::Telemetry()> telemetry)
    : local_(std::move(local)), peers_(std::move(peers)), factory_(std::move(factory)), cas_(std::move(cas)),
      merkle_(std::move(merkle)), local_id_(std::move(local_node_id)), telemetry_(std::move(telemetry)),
      shared_(std::make_shared<Shared>()) {
    if (!peers_ || !factory_ || !cas_ || !merkle_) throw std::invalid_argument("Placer: missing dependency");
    peers_->RegisterPreemptCallback(
        [this](const std::string& node_id, const std::string& reason) { OnPeerDrain(node_id, reason); });
}

void Placer::SetHazardModel(std::shared_ptr<const HazardModel> model) {
    std::lock_guard lock(hazard_mu_);
    hazard_ = std::move(model);
}

std::shared_ptr<const HazardModel> Placer::Hazard() const {
    std::lock_guard lock(hazard_mu_);
    return hazard_;
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
    const int64_t on_peer = std::min(total_bytes, prev + std::max<int64_t>(0, bytes_pushed));
    push_cache_[key] = on_peer;
    push_cache_lru_.remove(key);
    push_cache_lru_.push_front(key);
    while (push_cache_.size() > kPushCacheMax) {
        const std::string evict = push_cache_lru_.back();
        push_cache_lru_.pop_back();
        push_cache_.erase(evict);
    }
}

std::map<std::string, Placer::NodeTraffic> Placer::Traffic() const {
    std::lock_guard lock(traffic_mu_);
    return traffic_;
}

void Placer::AddTraffic(const std::string& node_id, uint64_t pushed, uint64_t pulled) {
    std::lock_guard lock(traffic_mu_);
    auto& t = traffic_[node_id];
    t.bytes_pushed += pushed;
    t.bytes_pulled += pulled;
}

std::shared_ptr<Placer::DonorSent> Placer::SentFor(const std::string& node_id) {
    std::lock_guard lock(sent_mu_);
    auto& slot = sent_[node_id];
    if (!slot) slot = std::make_shared<DonorSent>();
    return slot;
}

Placer::~Placer() {
    if (hedge_thread_.joinable()) {
        hedge_thread_.request_stop();
        hedge_thread_.join();
    }
    std::vector<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard lock(shared_->mu);
        for (auto& [id, t] : shared_->tasks) tasks.push_back(t);
    }
    for (auto& t : tasks) {
        if (!t->IsLocal()) Cancel(t->id);
    }
    std::lock_guard lock(workers_mu_);
    workers_.clear();
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
    std::erase_if(workers_, [](const Worker& w) { return w.task->worker_done.load(); });
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

void Placer::Execute(const std::string& task_id, const v1::TaskSpec& spec, node::TaskCallbacks cb) {
    auto task = std::make_shared<Task>();
    task->id = task_id;
    task->spec = spec;
    task->cb = std::move(cb);
    task->on_finished = [this, weak = std::weak_ptr<Task>(task)] {
        if (auto t = weak.lock()) CancelHedges(t);
    };
    {
        std::lock_guard lock(shared_->mu);
        shared_->tasks[task_id] = task;
    }
    LaunchWorker(task);
}

void Placer::LaunchWorker(const std::shared_ptr<Task>& task) {
    std::lock_guard lock(workers_mu_);
    ReapFinishedLocked();
    workers_.push_back(Worker{task, std::jthread([this, task] {
                                  try {
                                      Place(task);
                                  } catch (const std::exception& e) {
                                      task->Done(Failed(local_id_, std::string("placer: ") + e.what()));
                                  }
                                  shared_->Forget(task->id);
                                  task->worker_done = true;
                              })});
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

bool PullOutputs(node::NodeClient& client, store::CasStore& cas, const v1::TaskResult& r, std::string* error,
                 uint64_t* bytes_received) {
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
        return client.Pull(cas, missing, error, bytes_received);
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

struct Reservation {
    std::string node_id;
    bool is_local{false};
};

} // namespace

void Placer::Place(const std::shared_ptr<Task>& task) {
    v1::TaskSpec spec = task->spec;
    const net::Telemetry self = telemetry_ ? telemetry_() : net::Telemetry{};
    if (spec.resources().arch().empty() && !self.arch.empty()) spec.mutable_resources()->set_arch(self.arch);

    const int max_retries = MaxRetries(spec);
    int attempt = 0;                       // completed failed attempts that consumed a retry
    std::unordered_set<std::string> skip = task->exclude; // nodes skipped after a retryable infra failure
    std::unordered_set<std::string> tried; // nodes already offered this placement round
    // Nodes that lacked the command, and when they may be offered again.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> missing_until;
    auto lacks_command = [&](const std::string& id) {
        auto it = missing_until.find(id);
        return it != missing_until.end() && std::chrono::steady_clock::now() < it->second;
    };
    std::string last_reason = "no eligible nodes";
    std::optional<v1::TaskResult> last_attempt_result; // last real run, if any
    std::optional<std::vector<v1::Digest>> needed;
    const std::string input_root_hash = spec.input_root().hash();
    const int64_t input_tree_bytes = InputTreeBytes(spec);

    // Seconds a donor must stay for this task to finish. The policy timeout is an upper bound,
    // not an estimate, so cap it; the hazard ranking only needs a horizon of the right scale.
    const double task_s = std::min<double>(spec.policy().has_timeout_s() && spec.policy().timeout_s() > 0
                                               ? spec.policy().timeout_s()
                                               : 3600,
                                           kHazardHorizonCapS);

    auto release = [&](const Reservation& res) {
        std::lock_guard lock(shared_->mu);
        shared_->AddLocked(res.node_id, -1);
        std::lock_guard tlock(task->mu);
        if (task->assigned_node == res.node_id) task->assigned_node.clear();
        task->client.reset();
        task->is_local = false;
        task->running = false;
    };

    auto blocked = [&](const std::string& id) { return skip.contains(id) || tried.contains(id) || lacks_command(id); };

    auto reserve_next = [&]() -> std::optional<Reservation> {
        std::lock_guard lock(shared_->mu);
        std::vector<Candidate> candidates;
        if (local_ && !blocked(local_id_)) {
            candidates.push_back(Candidate{.node_id = local_id_,
                                           .is_local = true,
                                           .cores_total = self.cores_total,
                                           .cores_free = self.cores_free,
                                           .mem_available = self.mem_available,
                                           .cpu_load_pct = self.cpu_load_pct,
                                           .on_ac = self.on_ac,
                                           .arch = self.arch,
                                           .latency = std::chrono::milliseconds(0),
                                           .missing_bytes = 0,
                                           .in_flight = shared_->InFlightLocked(local_id_)});
        }
        const auto hazard = Hazard();
        const bool use_hazard = hazard && hazard->HasHistory();
        for (const auto& p : peers_->ListActive()) {
            if (p.id == local_id_ || blocked(p.id)) continue;
            Candidate cand{.node_id = p.id,
                           .is_local = false,
                           .cores_total = p.cores,
                           .cores_free = p.cores_free,
                           .mem_available = p.mem_available,
                           .cpu_load_pct = p.cpu_load_pct,
                           .on_ac = p.is_charging,
                           .arch = p.arch,
                           .latency = p.latency,
                           .missing_bytes = EstimatedMissingBytes(p.id, input_root_hash, input_tree_bytes),
                           .in_flight = shared_->InFlightLocked(p.id),
                           .bandwidth_bytes_per_s = bandwidth_.Measured(p.id).value_or(0.0)};
            if (use_hazard) cand.expected_min_s = hazard->ExpectedMin(p.id, task_s);
            candidates.push_back(std::move(cand));
        }
        if (use_hazard) {
            // This node outlives its own tasks: it is the requester.
            for (auto& c : candidates) {
                if (c.is_local) c.expected_min_s = task_s;
            }
        }
        for (const Candidate& c : Rank(spec, std::move(candidates), input_tree_bytes)) {
            shared_->AddLocked(c.node_id, 1);
            {
                std::lock_guard tlock(task->mu);
                task->assigned_node = c.node_id;
                task->is_local = c.is_local;
            }
            return Reservation{c.node_id, c.is_local};
        }
        return std::nullopt;
    };

    auto begin_retry = [&](const std::string& node, const std::string& reason) {
        ++attempt;
        skip.insert(node);
        tried.clear();
        task->drain_preempted = false; // fresh attempt
        const std::string detail =
            "retry " + std::to_string(attempt) + "/" + std::to_string(max_retries) + ": " + reason;
        if (task->cb.on_state) task->cb.on_state(v1::QUEUED, detail);
    };

    auto fail_placement = [&](std::string reason) {
        if (last_attempt_result) return task->Done(std::move(*last_attempt_result));
        return task->Done(Failed(local_id_, "no node can run this task: " + std::move(reason)));
    };

    auto infra_retry_or_fail = [&](const std::string& node, const std::string& reason) -> bool {
        last_reason = reason;
        if (attempt < max_retries) {
            begin_retry(node, reason);
            return true;
        }
        return false;
    };

    // True when some node could take this task if nothing were in flight (capacity wait vs hard miss).
    auto any_eligible = [&] {
        std::vector<Candidate> candidates;
        if (local_ && !skip.contains(local_id_) && !lacks_command(local_id_)) {
            candidates.push_back(Candidate{.node_id = local_id_,
                                           .is_local = true,
                                           .cores_total = self.cores_total,
                                           .cores_free = self.cores_free,
                                           .mem_available = self.mem_available,
                                           .cpu_load_pct = self.cpu_load_pct,
                                           .on_ac = self.on_ac,
                                           .arch = self.arch,
                                           .missing_bytes = 0,
                                           .in_flight = 0});
        }
        for (const auto& p : peers_->ListActive()) {
            if (p.id == local_id_ || skip.contains(p.id) || lacks_command(p.id)) continue;
            candidates.push_back(
                Candidate{.node_id = p.id,
                          .is_local = false,
                          .cores_total = p.cores,
                          .cores_free = p.cores_free,
                          .mem_available = p.mem_available,
                          .cpu_load_pct = p.cpu_load_pct,
                          .on_ac = p.is_charging,
                          .arch = p.arch,
                          .latency = p.latency,
                          .missing_bytes = EstimatedMissingBytes(p.id, input_root_hash, input_tree_bytes),
                          .in_flight = 0});
        }
        return !Rank(spec, std::move(candidates), input_tree_bytes).empty();
    };

    // Donor drain may arrive as MarkDraining, or only as a cancelled TaskResult if the
    // preempt callback raced behind CancelPreemptible. Treat PeerState::Draining as drain.
    auto donor_draining = [&](const std::string& node_id) {
        if (task->drain_preempted.load()) return true;
        if (auto p = peers_->Get(node_id); p && p->state == net::PeerState::Draining) {
            task->drain_preempted = true;
            return true;
        }
        return false;
    };

    while (true) {
        // User/job cancel always wins over drain preemption.
        if (task->cancelled.load()) return task->Done(Cancelled(local_id_));

        auto reserved = reserve_next();
        if (!reserved) {
            // A hedge only takes a slot that is free right now; it never queues behind real work.
            if (task->is_hedge) return task->Done(Cancelled(local_id_));
            // Wait for in-flight capacity; timeout_s is the run timeout, not a placement cap.
            if (!any_eligible()) {
                return fail_placement(last_reason);
            }
            tried.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        tried.insert(reserved->node_id);
        if (task->is_hedge && !task->hedge_counted.exchange(true)) {
            std::lock_guard hedge_lock(hedge_mu_);
            ++hedge_stats_.launched;
        }

        if (reserved->is_local) {
            if (!local_) {
                release(*reserved);
                if (infra_retry_or_fail(reserved->node_id, "local executor unavailable")) continue;
                return fail_placement(last_reason);
            }
            auto result = RunLocal(task, spec);
            release(*reserved);
            last_attempt_result = result;
            const bool user_cancel = task->cancelled.load();
            const bool drain = donor_draining(reserved->node_id);
            if (user_cancel) return task->Done(Cancelled(local_id_));
            if (IsRetryable(result, drain, user_cancel) && attempt < max_retries) {
                begin_retry(reserved->node_id, RetryReason(result, drain));
                continue;
            }
            return task->Done(std::move(result));
        }

        auto peer = peers_->Get(reserved->node_id);
        auto client = peer ? factory_(*peer) : nullptr;
        if (!client) {
            release(*reserved);
            const std::string reason = "node " + reserved->node_id + ": not reachable";
            if (infra_retry_or_fail(reserved->node_id, reason)) continue;
            return fail_placement(reason);
        }
        if (!needed) {
            needed.emplace();
            std::unordered_set<std::string> seen;
            auto add_digest = [&](const store::Digest& d) {
                if (d.hash.empty() || !seen.insert(d.hash).second) return;
                needed->push_back(node::ToProto(d));
            };
            for (const auto& d : merkle_->CollectTree(node::FromProto(spec.input_root()))) {
                add_digest(d);
            }
            if (spec.env_spec().name() == "mirror") {
                auto it = spec.env_spec().params().find("root_digest");
                if (it != spec.env_spec().params().end() && store::CasStore::IsValidHash(it->second)) {
                    store::Digest root;
                    root.hash = it->second;
                    if (auto bytes = cas_->GetBlob(it->second)) {
                        root.size_bytes = static_cast<int64_t>(bytes->size());
                    }
                    add_digest(root);
                }
            }
        }
        {
            std::lock_guard lock(task->mu);
            task->client = client;
        }

        std::string err;
        if (node::EncodedOfferByteSize(task->id, spec, *needed) > static_cast<size_t>(node::kNodeApiMaxMessageSize)) {
            release(*reserved);
            return fail_placement("input tree too large to offer: " + std::to_string(needed->size()) + " digests");
        }
        auto reply = client->Offer(task->id, spec, *needed, &err);
        if (!reply) {
            release(*reserved);
            const std::string reason = "node " + reserved->node_id + ": " + err;
            if (infra_retry_or_fail(reserved->node_id, reason)) continue;
            return fail_placement(reason);
        }
        if (!reply->accepted()) {
            release(*reserved);
            const std::string& why = reply->reason();
            if (OfferRejectionRetryable(why)) {
                tried.erase(reserved->node_id);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            } else if (OfferRejectionOwnerBusy(why)) {
                missing_until[reserved->node_id] = std::chrono::steady_clock::now() + kOwnerBusyRecheck;
                last_reason = "node " + reserved->node_id + ": " + why;
            } else if (OfferRejectionTryNextPeer(why)) {
                missing_until[reserved->node_id] = std::chrono::steady_clock::now() + kCapabilityRecheck;
                last_reason = "node " + reserved->node_id + ": " + why;
            } else {
                return task->Done(
                    Failed(local_id_, "no node can run this task: node " + reserved->node_id + ": " + why));
            }
            continue;
        }
        if (task->cb.on_state) task->cb.on_state(v1::PLACED, "node " + reserved->node_id);

        std::vector<v1::Digest> missing(reply->missing().begin(), reply->missing().end());
        if (needed) {
            // Everything it did not report missing is already there: a future seeding source.
            std::unordered_set<std::string> missing_hashes;
            for (const auto& d : missing) missing_hashes.insert(d.hash());
            std::vector<v1::Digest> held;
            for (const auto& d : *needed) {
                if (!missing_hashes.contains(d.hash())) held.push_back(d);
            }
            NoteHolders(reserved->node_id, held);
        }
        uint64_t wire_pushed = 0;
        bool pushed_ok = true;
        {
            // One push at a time per donor: whatever a sibling task delivered while we waited
            // is not sent again.
            auto donor = SentFor(reserved->node_id);
            std::lock_guard push_lock(donor->push_mu);
            const auto now = std::chrono::steady_clock::now();
            std::vector<v1::Digest> to_send;
            {
                std::lock_guard lock(donor->mu);
                std::erase_if(donor->sent, [&](const auto& kv) { return now - kv.second > kSentTtl; });
                for (const auto& d : missing) {
                    if (!donor->sent.contains(d.hash())) to_send.push_back(d);
                }
            }
            // Let the donor fetch what other donors hold; push only the rest.
            const std::vector<v1::Digest> wanted = to_send;
            to_send = SeedFromPeers(reserved->node_id, *client, std::move(to_send));
            {
                std::unordered_set<std::string> still_missing;
                for (const auto& d : to_send) still_missing.insert(d.hash());
                std::lock_guard lock(donor->mu);
                for (const auto& d : wanted) {
                    if (!still_missing.contains(d.hash())) donor->sent[d.hash()] = std::chrono::steady_clock::now();
                }
            }
            const auto push_start = std::chrono::steady_clock::now();
            pushed_ok = client->Push(*cas_, to_send, &err, &wire_pushed);
            if (pushed_ok) bandwidth_.Observe(reserved->node_id, wire_pushed, std::chrono::steady_clock::now() - push_start);
            if (pushed_ok) {
                {
                    std::lock_guard lock(donor->mu);
                    for (const auto& d : to_send) donor->sent[d.hash()] = std::chrono::steady_clock::now();
                }
                NoteHolders(reserved->node_id, to_send);
            }
        }
        AddTraffic(reserved->node_id, wire_pushed, 0);
        if (!pushed_ok) {
            release(*reserved);
            const std::string reason = "node " + reserved->node_id + ": uploading inputs failed: " + err;
            if (infra_retry_or_fail(reserved->node_id, reason)) continue;
            return fail_placement(reason);
        }
        {
            int64_t pushed = 0;
            for (const auto& d : missing) pushed += std::max<int64_t>(d.size_bytes(), 0);
            if (missing.empty()) pushed = input_tree_bytes;
            NoteInputsPushed(reserved->node_id, input_root_hash, pushed, input_tree_bytes);
        }
        if (task->cancelled.load()) {
            release(*reserved);
            return task->Done(Cancelled(local_id_));
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
        {
            std::lock_guard lock(task->mu);
            task->running = true;
            task->ever_ran = true;
            task->run_started = std::chrono::steady_clock::now();
        }
        auto result = client->Run(task->id, on_event, &err);
        const bool user_cancel = task->cancelled.load();
        const bool drain = donor_draining(reserved->node_id);

        if (!result) {
            release(*reserved);
            if (user_cancel) return task->Done(Cancelled(reserved->node_id));
            v1::TaskResult lost =
                Failed(reserved->node_id, "node " + reserved->node_id + " lost during run (" + err + ")");
            last_attempt_result = lost;
            if (attempt < max_retries) {
                begin_retry(reserved->node_id, lost.error());
                continue;
            }
            return task->Done(std::move(lost));
        }
        result->set_executed_by(reserved->node_id);
        last_attempt_result = *result;

        // Decide retry before pulling outputs so partial donor outputs stay discarded.
        if (IsRetryable(*result, drain, user_cancel) && attempt < max_retries) {
            release(*reserved);
            begin_retry(reserved->node_id, RetryReason(*result, drain));
            continue;
        }
        if (user_cancel || result->cancelled()) {
            release(*reserved);
            return task->Done(Cancelled(reserved->node_id));
        }
        uint64_t wire_pulled = 0;
        const auto pull_start = std::chrono::steady_clock::now();
        const bool pulled_ok = PullOutputs(*client, *cas_, *result, &err, &wire_pulled);
        if (pulled_ok) bandwidth_.Observe(reserved->node_id, wire_pulled, std::chrono::steady_clock::now() - pull_start);
        AddTraffic(reserved->node_id, 0, wire_pulled);
        if (!pulled_ok) {
            release(*reserved);
            v1::TaskResult lost =
                Failed(reserved->node_id, "node " + reserved->node_id + " lost while sending outputs: " + err);
            last_attempt_result = lost;
            if (attempt < max_retries) {
                begin_retry(reserved->node_id, lost.error());
                continue;
            }
            return task->Done(std::move(lost));
        }
        release(*reserved);
        return task->Done(std::move(*result));
    }
}

Placer::HedgeConfig Placer::HedgeConfig::FromPolicy(const node::DonorPolicy& policy) {
    HedgeConfig c;
    c.max_copies = policy.max_copies;
    return c;
}

void Placer::SetHedging(HedgeConfig cfg) {
    const bool active = static_cast<bool>(cfg.survival);
    {
        std::lock_guard lock(hedge_mu_);
        hedge_cfg_ = std::move(cfg);
    }
    if (!active || hedge_thread_.joinable()) return;
    hedge_thread_ = std::jthread([this](std::stop_token st) {
        while (!st.stop_requested()) {
            std::chrono::milliseconds interval;
            {
                std::lock_guard lock(hedge_mu_);
                interval = hedge_cfg_.check_interval;
            }
            std::this_thread::sleep_for(interval);
            if (st.stop_requested()) break;
            try {
                HedgeScan();
            } catch (const std::exception&) {
                // a failed scan only delays hedging; the primary copy is unaffected
            }
        }
    });
}

Placer::HedgeStats Placer::Hedging() const {
    std::lock_guard lock(hedge_mu_);
    return hedge_stats_;
}

namespace {

double HedgeHorizon(double default_horizon_s, const v1::TaskSpec& spec) {
    double h = default_horizon_s;
    if (spec.policy().has_timeout_s() && spec.policy().timeout_s() > 0) {
        h = std::min(h, static_cast<double>(spec.policy().timeout_s()));
    }
    return h;
}

} // namespace

void Placer::HedgeScan() {
    HedgeConfig cfg;
    {
        std::lock_guard lock(hedge_mu_);
        cfg = hedge_cfg_;
    }
    if (!cfg.survival || cfg.max_copies < 2) return;

    struct Pick {
        std::shared_ptr<Task> task;
        std::string node;
        std::unordered_set<std::string> exclude;
    };
    std::vector<Pick> picks;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(shared_->mu);
        int unplaced = 0;
        int running = 0;
        for (auto& [id, t] : shared_->tasks) {
            if (t->is_hedge || t->finished.load()) continue;
            std::lock_guard tlock(t->mu);
            if (t->assigned_node.empty()) {
                ++unplaced;
                continue;
            }
            ++running;
            if (t->is_local || !t->running || now < t->next_hedge) continue;
            int live = 0;
            bool pending = false; // a hedge was launched but has no donor slot yet
            std::unordered_set<std::string> exclude{t->assigned_node};
            for (auto& h : t->hedges) {
                if (h->finished.load()) continue;
                ++live;
                std::lock_guard hlock(h->mu);
                if (h->assigned_node.empty() && !h->ever_ran) pending = true;
                if (!h->assigned_node.empty()) exclude.insert(h->assigned_node);
            }
            if (pending || live + 1 >= cfg.max_copies) continue;
            picks.push_back(Pick{t, t->assigned_node, std::move(exclude)});
        }
        // The tail: nothing is waiting for a slot and few tasks are still running.
        if (unplaced > 0 || running > cfg.tail_max_running) return;
    }

    for (auto& pick : picks) {
        const double p = cfg.survival(pick.node, HedgeHorizon(cfg.default_horizon_s, pick.task->spec));
        if (!(p < cfg.survive_below)) continue; // also skips NaN
        LaunchHedge(pick.task, std::move(pick.exclude), cfg);
    }
}

void Placer::LaunchHedge(const std::shared_ptr<Task>& primary, std::unordered_set<std::string> exclude,
                         const HedgeConfig& cfg) {
    // Only start a copy if some other node could take it; otherwise wait for the next scan.
    bool target = false;
    for (const auto& p : peers_->ListActive()) {
        if (p.id != local_id_ && !exclude.contains(p.id)) target = true;
    }
    const auto& policy = primary->spec.policy();
    const bool may_local = !policy.has_may_run_local() || policy.may_run_local();
    if (local_ && may_local && !exclude.contains(local_id_)) target = true;
    if (!target) return;

    auto hedge = std::make_shared<Task>();
    hedge->is_hedge = true;
    hedge->spec = primary->spec;
    hedge->spec.mutable_policy()->set_max_retries(0);
    hedge->exclude = std::move(exclude);
    {
        std::lock_guard lock(primary->mu);
        if (primary->finished.load()) return;
        hedge->id = primary->id + "#hedge" + std::to_string(++primary->hedge_seq);
        primary->next_hedge = std::chrono::steady_clock::now() + cfg.retry_after;
        primary->hedges.push_back(hedge);
    }
    hedge->cb.on_done = [this, primary_weak = std::weak_ptr<Task>(primary),
                         hedge_weak = std::weak_ptr<Task>(hedge)](v1::TaskResult r) {
        auto h = hedge_weak.lock();
        if (h) OnHedgeDone(primary_weak.lock(), h, std::move(r));
    };
    {
        std::lock_guard lock(shared_->mu);
        shared_->tasks[hedge->id] = hedge;
    }
    LaunchWorker(hedge);
}

void Placer::OnHedgeDone(const std::shared_ptr<Task>& primary, const std::shared_ptr<Task>& hedge,
                         v1::TaskResult r) {
    double cpu = 0;
    bool ran = false;
    {
        std::lock_guard lock(hedge->mu);
        if (hedge->ever_ran) {
            const std::chrono::duration<double> ran_for = std::chrono::steady_clock::now() - hedge->run_started;
            cpu = ran_for.count() * std::max(1, hedge->spec.resources().cores());
            ran = true;
        }
    }
    const bool usable = !r.cancelled() && !r.timed_out() && r.exit_code() == 0 && r.error().empty();
    bool won = false;
    if (usable && primary) {
        won = primary->Finish(r);
        if (won) Cancel(primary->id); // stop the original copy; it is no longer needed
    }
    const bool counted = hedge->hedge_counted.load();
    std::lock_guard lock(hedge_mu_);
    if (won) ++hedge_stats_.won;
    if (counted && !won && r.cancelled()) ++hedge_stats_.cancelled;
    if (ran) {
        hedge_stats_.cpu_seconds += cpu;
        if (!won) hedge_stats_.wasted_cpu_seconds += cpu;
    }
}

void Placer::CancelHedges(const std::shared_ptr<Task>& primary) {
    std::vector<std::shared_ptr<Task>> hedges;
    {
        std::lock_guard lock(primary->mu);
        hedges = primary->hedges;
    }
    for (const auto& h : hedges) {
        if (!h->finished.load()) Cancel(h->id);
    }
}

void Placer::SetSeeding(SeedConfig cfg) {
    seed_gate_.Configure(cfg.per_source_cap, cfg.lan_cap);
    std::lock_guard lock(seed_mu_);
    seed_cfg_ = cfg;
}

void Placer::AddSeeded(const std::string& node_id, uint64_t bytes) {
    if (bytes == 0) return;
    std::lock_guard lock(traffic_mu_);
    traffic_[node_id].bytes_seeded += bytes;
}

void Placer::NoteHolders(const std::string& node_id, const std::vector<v1::Digest>& digests) {
    if (node_id == local_id_ || digests.empty()) return;
    std::lock_guard lock(holders_mu_);
    if (holders_.size() > kHoldersMaxBlobs) holders_.clear();
    for (const auto& d : digests) {
        auto& nodes = holders_[d.hash()];
        if (std::find(nodes.begin(), nodes.end(), node_id) != nodes.end()) continue;
        if (nodes.size() >= kHoldersPerBlob) nodes.erase(nodes.begin());
        nodes.push_back(node_id);
    }
}

std::vector<v1::Digest> Placer::SeedFromPeers(const std::string& target, node::NodeClient& client,
                                              std::vector<v1::Digest> missing) {
    SeedConfig cfg;
    {
        std::lock_guard lock(seed_mu_);
        cfg = seed_cfg_;
    }
    if (!cfg.enabled || missing.empty()) return missing;

    std::vector<std::vector<std::string>> holders(missing.size());
    {
        std::lock_guard lock(holders_mu_);
        for (size_t i = 0; i < missing.size(); ++i) {
            if (auto it = holders_.find(missing[i].hash()); it != holders_.end()) holders[i] = it->second;
        }
    }

    // The least-busy holder per blob: fewest seeds in flight from it, then fewest of our tasks on it.
    std::unordered_map<std::string, int> in_flight;
    auto busyness = [&](const std::string& n) {
        auto [it, inserted] = in_flight.try_emplace(n, 0);
        if (inserted) it->second = InFlightOn(n);
        return seed_gate_.Active(n) * 1000 + it->second;
    };
    std::map<std::string, std::vector<v1::Digest>> by_source;
    std::vector<v1::Digest> rest;
    for (size_t i = 0; i < missing.size(); ++i) {
        std::string best;
        int best_busy = 0;
        for (const auto& n : holders[i]) {
            if (n == target || n == local_id_ || !peers_->Get(n)) continue;
            const int b = busyness(n);
            if (best.empty() || b < best_busy) {
                best = n;
                best_busy = b;
            }
        }
        if (best.empty())
            rest.push_back(missing[i]);
        else
            by_source[best].push_back(missing[i]);
    }

    for (auto& [source, group] : by_source) {
        auto token = seed_gate_.Acquire(source, cfg.acquire_timeout);
        if (!token) { // every slot is taken: the requester sends these itself
            rest.insert(rest.end(), group.begin(), group.end());
            continue;
        }
        std::vector<v1::Digest> fetched;
        std::string err;
        client.SeedBlobs(source, group, &fetched, &err); // `fetched` is authoritative even on error
        std::unordered_set<std::string> got;
        for (const auto& f : fetched) got.insert(f.hash());
        std::vector<v1::Digest> have;
        uint64_t bytes = 0;
        for (const auto& d : group) {
            if (got.contains(d.hash())) {
                have.push_back(d);
                bytes += static_cast<uint64_t>(std::max<int64_t>(d.size_bytes(), 0));
            } else {
                rest.push_back(d);
            }
        }
        NoteHolders(target, have);
        AddSeeded(target, bytes);
    }
    return rest;
}

} // namespace zeromesh::sched
