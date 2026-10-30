#include "sched/placer.hpp"

#include "exec/units.hpp"
#include "node/convert.hpp"
#include "node/departure.hpp"
#include "sched/rank.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>
#include <map>
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

} // namespace

struct Placer::Task {
    std::string id;
    v1::TaskSpec spec;
    node::TaskCallbacks cb;
    std::atomic<bool> cancelled{false};       // user/job cancel: final
    std::atomic<bool> drain_preempted{false}; // donor drain: retryable cancel
    std::atomic<bool> finished{false};
    std::atomic<bool> worker_done{false};

    std::mutex mu;
    bool is_local{false};
    std::string assigned_node; // node currently reserved / running
    std::shared_ptr<node::NodeClient> client;

    // Partial commit: units the task has committed so far (their blobs are in our CAS). The
    // next attempt gets them under .zm_units/ in its input, so it only redoes the rest.
    std::mutex units_mu;
    std::map<std::string, v1::Digest> units;
    bool units_dirty{false};
    std::function<void(const std::string&, const v1::Digest&)> unit_sink;

    bool IsLocal() {
        std::lock_guard lock(mu);
        return is_local;
    }

    void Done(v1::TaskResult r) {
        if (finished.exchange(true)) return;
        if (cb.on_done) cb.on_done(std::move(r));
    }
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

void Placer::OnPeerDrain(const std::string& node_id, const std::string& reason) {
    // A freeze-first donor keeps our task frozen (and kills it itself once its ttl runs out, which
    // we then retry as a drain). Cancelling now would throw away the progress it is protecting.
    if (node::ParseDepartureReason(reason)) return;
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
    {
        std::lock_guard lock(shared_->mu);
        shared_->tasks[task_id] = task;
    }
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
    cb.on_unit = task->unit_sink;
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

    // Units reach the task through here, from a donor's event stream or from a local run. The
    // sink never outlives Place(), which is the only thing that runs attempts of this task.
    Task* const task_raw = task.get();
    task->unit_sink = [task_raw](const std::string& unit_id, const v1::Digest& digest) {
        {
            std::lock_guard lock(task_raw->units_mu);
            task_raw->units[unit_id] = digest;
            task_raw->units_dirty = true;
        }
        if (task_raw->cb.on_unit) task_raw->cb.on_unit(unit_id, digest);
    };

    const int max_retries = MaxRetries(spec);
    int attempt = 0;                       // completed failed attempts that consumed a retry
    std::unordered_set<std::string> skip;  // nodes skipped after a retryable infra failure
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

    auto release = [&](const Reservation& res) {
        std::lock_guard lock(shared_->mu);
        shared_->AddLocked(res.node_id, -1);
        std::lock_guard tlock(task->mu);
        if (task->assigned_node == res.node_id) task->assigned_node.clear();
        task->client.reset();
        task->is_local = false;
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
        for (const auto& p : peers_->ListActive()) {
            if (p.id == local_id_ || blocked(p.id)) continue;
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
                          .in_flight = shared_->InFlightLocked(p.id)});
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

        // A retry starts from the units the last attempt committed.
        {
            std::lock_guard ulock(task->units_mu);
            if (task->units_dirty) {
                task->units_dirty = false;
                try {
                    store::Digest root = node::FromProto(task->spec.input_root());
                    for (const auto& [unit_id, digest] : task->units) {
                        root = merkle_->Overlay(root, std::string(exec::kUnitsDirName) + "/" + unit_id,
                                                node::FromProto(digest), /*item_is_tree=*/false);
                    }
                    *spec.mutable_input_root() = node::ToProto(root);
                    needed.reset(); // the input tree changed: offer the new one
                } catch (const std::exception&) {
                    // Units only save work; without them the attempt starts from scratch.
                }
            }
        }

        auto reserved = reserve_next();
        if (!reserved) {
            // Wait for in-flight capacity; timeout_s is the run timeout, not a placement cap.
            if (!any_eligible()) {
                return fail_placement(last_reason);
            }
            tried.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        tried.insert(reserved->node_id);

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
            pushed_ok = client->Push(*cas_, to_send, &err, &wire_pushed);
            if (pushed_ok) {
                std::lock_guard lock(donor->mu);
                for (const auto& d : to_send) donor->sent[d.hash()] = std::chrono::steady_clock::now();
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
            if (ev.has_unit_committed()) {
                const auto& u = ev.unit_committed();
                const std::string& unit_hash = u.digest().hash();
                if (exec::IsValidUnitId(u.unit_id()) && store::CasStore::IsValidHash(unit_hash) && task->unit_sink) {
                    // Fetch it now: if this donor is lost next, the unit must already be safe here.
                    std::string unit_err;
                    uint64_t unit_bytes = 0;
                    const bool have = cas_->HasBlob(unit_hash) ||
                                      client->Pull(*cas_, std::vector<v1::Digest>{u.digest()}, &unit_err, &unit_bytes);
                    AddTraffic(reserved->node_id, 0, unit_bytes);
                    if (have) task->unit_sink(u.unit_id(), u.digest());
                }
                return;
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
        const bool pulled_ok = PullOutputs(*client, *cas_, *result, &err, &wire_pulled);
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

} // namespace zeromesh::sched
