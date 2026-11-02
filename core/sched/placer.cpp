#include "sched/placer.hpp"

#include "node/convert.hpp"
#include "node/departure.hpp"
#include "sched/rank.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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

node::BatchDone Done(const std::string& node, v1::FailureKind kind, std::string message, bool cancelled = false) {
    node::BatchDone d;
    d.node = node;
    d.failure = kind;
    d.message = std::move(message);
    d.cancelled = cancelled;
    return d;
}

// What to do about a donor that rejected an offer.
struct Rejection {
    bool passing{false}; // true: the donor will take work again soon
    std::chrono::milliseconds retry_after{0};
};

constexpr std::chrono::milliseconds kMinRetryAfter{50};

Rejection ClassifyRejection(const v1::OfferReply& reply) {
    switch (reply.reject_kind()) {
    case v1::REJECT_DRAINING:
    case v1::REJECT_BUSY:
        return {true, std::max(std::chrono::milliseconds(reply.retry_after_ms()), kMinRetryAfter)};
    default:
        return {false, std::chrono::milliseconds(0)};
    }
}

// Longest batch horizon (seconds) used when ranking donors by departure hazard.
constexpr double kHazardHorizonCapS = 900.0;

// A batch waiting for capacity looks again at least this often.
constexpr std::chrono::milliseconds kPlacementRecheck{100};

double BatchSeconds(const v1::BatchSpec& spec) {
    const double per_item = spec.timeout_s() > 0 ? spec.timeout_s() : 3600;
    return std::min(per_item * std::max(spec.items_size(), 1), kHazardHorizonCapS);
}

sched::Need NeedOf(const v1::BatchSpec& spec) {
    sched::Need n;
    n.cores = std::max(spec.resources().cores(), 1);
    n.mem_bytes = spec.resources().mem_bytes();
    n.gpus = spec.resources().gpus();
    n.arch = spec.resources().arch();
    n.may_local = spec.where() != v1::REMOTE_ONLY;
    n.may_remote = spec.where() != v1::LOCAL_ONLY;
    return n;
}

} // namespace

struct Placer::Batch {
    std::string id;
    v1::BatchSpec spec;
    node::BatchCallbacks cb;
    std::set<std::string> exclude;            // nodes this attempt must not use
    std::atomic<bool> cancelled{false};       // the caller cancelled
    std::atomic<bool> drain_preempted{false}; // the donor drained
    std::atomic<bool> finished{false};
    std::atomic<bool> worker_done{false};

    // Tail hedging. A hedge is an extra copy of `primary`, registered under its own id; its items
    // go to the primary's caller, and its end only matters if it finished the batch.
    bool is_hedge{false};
    std::atomic<bool> hedge_counted{false};

    std::mutex mu;
    bool is_local{false};
    std::string assigned_node; // node currently reserved / running
    std::shared_ptr<node::NodeClient> client;
    bool running{false};
    bool ever_ran{false};
    std::chrono::steady_clock::time_point run_started{};
    std::vector<std::shared_ptr<Batch>> hedges; // primary only
    int hedge_seq{0};
    std::chrono::steady_clock::time_point next_hedge{};
    std::function<void()> on_finished; // runs once after the batch's end was delivered
    // A primary whose own copy failed while hedges still ran: their outcome decides.
    std::optional<node::BatchDone> deferred;

    bool IsLocal() {
        std::lock_guard lock(mu);
        return is_local;
    }

    // True when this call delivered the end (the first one wins).
    bool Finish(node::BatchDone d) {
        if (finished.exchange(true)) return false;
        std::function<void()> hook;
        {
            std::lock_guard lock(mu);
            hook = std::move(on_finished);
            on_finished = nullptr;
        }
        if (cb.on_done) cb.on_done(std::move(d));
        if (hook) hook();
        return true;
    }
};

struct Placer::Shared {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<Batch>> batches;
    std::map<std::string, int> in_flight;         // node id -> batches we run there
    std::map<std::string, int64_t> mem_in_flight; // node id -> memory those batches declared
    std::map<std::string, int> gpus_in_flight;    // node id -> GPUs those batches declared
    std::condition_variable cv;                   // notified when a reservation is released

    int InFlightLocked(const std::string& node) const {
        auto it = in_flight.find(node);
        return it == in_flight.end() ? 0 : it->second;
    }
    int64_t MemInFlightLocked(const std::string& node) const {
        auto it = mem_in_flight.find(node);
        return it == mem_in_flight.end() ? 0 : it->second;
    }
    int GpusInFlightLocked(const std::string& node) const {
        auto it = gpus_in_flight.find(node);
        return it == gpus_in_flight.end() ? 0 : it->second;
    }
    void AddLocked(const std::string& node, int delta, int64_t mem, int gpus) {
        if ((in_flight[node] += delta) <= 0) in_flight.erase(node);
        if ((mem_in_flight[node] += delta * mem) <= 0) mem_in_flight.erase(node);
        if ((gpus_in_flight[node] += delta * gpus) <= 0) gpus_in_flight.erase(node);
    }
    void Forget(const std::string& id) {
        std::lock_guard lock(mu);
        batches.erase(id);
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

int Placer::SlotsFor(const v1::Resources& resources, v1::Where where) const {
    const int cores = std::max(resources.cores(), 1);
    int slots = 0;
    if (local_ && where != v1::REMOTE_ONLY) slots += local_->CoreCapacity() / cores;
    if (where != v1::LOCAL_ONLY) {
        for (const auto& p : peers_->ListActive()) {
            if (p.id == local_id_) continue;
            int donated = std::max(p.cores - 1, 1);
            if (const int limit = CoreLimitOf(p.id); limit >= 0) donated = limit;
            if (resources.gpus() > 0 && p.gpus < resources.gpus()) continue;
            slots += donated / cores;
        }
    }
    return std::max(slots, 1);
}

void Placer::SetHazardModel(std::shared_ptr<const HazardModel> model) {
    std::lock_guard lock(hazard_mu_);
    hazard_ = std::move(model);
}

std::shared_ptr<const HazardModel> Placer::Hazard() const {
    std::lock_guard lock(hazard_mu_);
    return hazard_;
}

void Placer::NoteCoreLimit(const std::string& node_id, const v1::OfferReply& reply) {
    if (!reply.has_core_limit()) return;
    std::lock_guard lock(limits_mu_);
    core_limit_[node_id] = static_cast<int>(reply.core_limit());
}

int Placer::CoreLimitOf(const std::string& node_id) const {
    std::lock_guard lock(limits_mu_);
    auto it = core_limit_.find(node_id);
    return it == core_limit_.end() ? -1 : it->second;
}

int64_t Placer::InputTreeBytes(const v1::BatchSpec& spec) const {
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
    std::vector<std::shared_ptr<Batch>> batches;
    {
        std::lock_guard lock(shared_->mu);
        for (auto& [id, b] : shared_->batches) batches.push_back(b);
    }
    for (auto& b : batches) Cancel(b->id);
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
    std::erase_if(workers_, [](const Worker& w) { return w.batch->worker_done.load(); });
}

void Placer::OnPeerDrain(const std::string& node_id, const std::string& reason) {
    // A freeze-first donor keeps our batch frozen (and kills it itself once its ttl runs out).
    // Cancelling now would throw away the progress it is protecting.
    if (node::ParseDepartureReason(reason)) return;
    std::vector<std::shared_ptr<Batch>> hit;
    {
        std::lock_guard lock(shared_->mu);
        for (auto& [id, b] : shared_->batches) {
            std::lock_guard block(b->mu);
            if (b->assigned_node == node_id && !b->is_local) hit.push_back(b);
        }
    }
    for (auto& b : hit) {
        b->drain_preempted = true;
        std::shared_ptr<node::NodeClient> client;
        {
            std::lock_guard lock(b->mu);
            client = b->client;
        }
        if (client) client->Cancel(b->id);
    }
}

void Placer::Execute(const std::string& batch_id, const v1::BatchSpec& spec, std::set<std::string> exclude,
                     node::BatchCallbacks cb) {
    auto batch = std::make_shared<Batch>();
    batch->id = batch_id;
    batch->spec = spec;
    batch->cb = std::move(cb);
    batch->exclude = std::move(exclude);
    batch->on_finished = [this, weak = std::weak_ptr<Batch>(batch)] {
        if (auto b = weak.lock()) CancelHedges(b);
    };
    {
        std::lock_guard lock(shared_->mu);
        shared_->batches[batch_id] = batch;
    }
    LaunchWorker(batch);
}

void Placer::LaunchWorker(const std::shared_ptr<Batch>& batch) {
    std::lock_guard lock(workers_mu_);
    ReapFinishedLocked();
    workers_.push_back(Worker{batch, std::jthread([this, batch] {
                                  try {
                                      Place(batch);
                                  } catch (const std::exception& e) {
                                      batch->Finish(Done(local_id_, v1::FAILURE_SANDBOX, std::string("placer: ") + e.what()));
                                  }
                                  shared_->Forget(batch->id);
                                  batch->worker_done = true;
                              })});
}

void Placer::Cancel(const std::string& batch_id) {
    std::shared_ptr<Batch> batch;
    {
        std::lock_guard lock(shared_->mu);
        auto it = shared_->batches.find(batch_id);
        if (it == shared_->batches.end()) return;
        batch = it->second;
    }
    batch->cancelled = true;
    bool is_local = false;
    std::shared_ptr<node::NodeClient> client;
    {
        std::lock_guard lock(batch->mu);
        is_local = batch->is_local;
        client = batch->client;
    }
    if (is_local) {
        if (local_) local_->Cancel(batch_id);
    } else if (client) {
        client->Cancel(batch_id);
    }
    shared_->cv.notify_all(); // a batch waiting for capacity notices at once
}

node::BatchDone Placer::RunLocal(const std::shared_ptr<Batch>& batch) {
    std::promise<node::BatchDone> done;
    auto fut = done.get_future();
    node::BatchCallbacks cb = batch->cb;
    cb.on_done = [&done](node::BatchDone d) { done.set_value(std::move(d)); };
    local_->Execute(batch->id, batch->spec, {}, std::move(cb));
    if (batch->cancelled) local_->Cancel(batch->id);
    return fut.get();
}


node::BatchDone Placer::RunRemote(const std::shared_ptr<Batch>& batch, const std::string& node_id,
                                  const std::shared_ptr<node::NodeClient>& client) {
    uint64_t pulled = 0;
    std::string pull_error;
    bool cancel_forwarded = false;
    auto on_event = [&](const v1::BatchEvent& ev) {
        if ((batch->cancelled || batch->drain_preempted) && !cancel_forwarded) {
            cancel_forwarded = true;
            client->Cancel(batch->id);
        }
        if (ev.has_started()) {
            if (batch->cb.on_started) batch->cb.on_started(ev.started());
        } else if (ev.has_log()) {
            if (batch->cb.on_log) batch->cb.on_log(ev.log());
        } else if (ev.has_done()) {
            // Fetch the item's outputs now: if this donor is lost next, the item is already safe here.
            const v1::ItemOutcome& o = ev.done();
            std::vector<v1::Digest> blobs{o.stdout(), o.stderr()}, trees;
            for (const auto& f : o.outputs()) {
                if (f.symlink()) continue;
                (f.is_dir() ? trees : blobs).push_back(f.digest());
            }
            std::string err;
            uint64_t bytes = 0;
            const bool ok = node::PullTrees(*client, *cas_, blobs, trees, &err, &bytes);
            pulled += bytes;
            AddTraffic(node_id, 0, bytes);
            if (ok) {
                if (batch->cb.on_item) batch->cb.on_item(o, node_id);
            } else if (pull_error.empty()) {
                pull_error = err; // the item is not reported: it will run again
            }
        } else if (ev.has_checkpoint()) {
            std::string err;
            uint64_t bytes = 0;
            if (node::PullTrees(*client, *cas_, {}, {ev.checkpoint().tree()}, &err, &bytes)) {
                if (batch->cb.on_checkpoint) batch->cb.on_checkpoint(ev.checkpoint().item(), ev.checkpoint().tree());
            }
            pulled += bytes;
            AddTraffic(node_id, 0, bytes);
        }
    };
    {
        std::lock_guard lock(batch->mu);
        batch->running = true;
        batch->ever_ran = true;
        batch->run_started = std::chrono::steady_clock::now();
    }
    std::string err;
    const auto start = std::chrono::steady_clock::now();
    auto finished = client->Run(batch->id, on_event, &err);
    if (pulled > 0) bandwidth_.Observe(node_id, pulled, std::chrono::steady_clock::now() - start);

    node::BatchDone d;
    d.node = node_id;
    d.bytes_pulled = pulled;
    if (batch->cancelled) {
        d.failure = v1::FAILURE_CANCELLED;
        d.cancelled = true;
        d.message = "cancelled";
    } else if (!finished) {
        d.failure = v1::FAILURE_DONOR_LEFT;
        d.message = "node " + node_id + " lost during the run (" + err + ")";
    } else if (finished->cancelled()) {
        d.failure = v1::FAILURE_DONOR_LEFT;
        d.message = batch->drain_preempted ? "node " + node_id + " drained" : "node " + node_id + " stopped the batch";
    } else if (!finished->error().empty()) {
        d.failure = v1::FAILURE_SANDBOX;
        d.message = "node " + node_id + ": " + finished->error();
    } else if (!pull_error.empty()) {
        d.failure = v1::FAILURE_DONOR_LEFT;
        d.message = "node " + node_id + " lost while sending outputs: " + pull_error;
    }
    return d;
}

void Placer::EndAttempt(const std::shared_ptr<Batch>& batch, node::BatchDone d) {
    if (!batch->is_hedge && d.failure != v1::FAILURE_NONE && !d.cancelled) {
        std::lock_guard lock(batch->mu);
        const bool hedging = std::any_of(batch->hedges.begin(), batch->hedges.end(),
                                         [](const auto& h) { return !h->finished.load(); });
        if (hedging) {
            batch->deferred = std::move(d); // a copy elsewhere may still finish the batch
            return;
        }
    }
    batch->Finish(std::move(d));
}

void Placer::Place(const std::shared_ptr<Batch>& batch) {
    v1::BatchSpec& spec = batch->spec;
    const net::Telemetry self = telemetry_ ? telemetry_() : net::Telemetry{};
    if (spec.resources().arch().empty() && !self.arch.empty()) spec.mutable_resources()->set_arch(self.arch);
    const sched::Need need = NeedOf(spec);
    const int64_t mem = std::max<int64_t>(need.mem_bytes, 0);
    const int gpus = std::max(need.gpus, 0);
    const std::string input_root_hash = spec.input_root().hash();
    const int64_t input_tree_bytes = InputTreeBytes(spec);
    const double batch_s = BatchSeconds(spec);

    std::set<std::string> skip = batch->exclude; // nodes this attempt will not use again
    std::set<std::string> tried;                 // offered this round
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> defer_until;
    std::map<std::string, std::pair<v1::RejectKind, std::string>> refused; // node -> why, for the failure
    auto deferred = [&](const std::string& id) {
        auto it = defer_until.find(id);
        return it != defer_until.end() && std::chrono::steady_clock::now() < it->second;
    };
    auto blocked = [&](const std::string& id) { return skip.contains(id) || tried.contains(id) || deferred(id); };

    auto candidates = [&](bool for_reservation) {
        std::vector<Candidate> out;
        if (local_ && !(for_reservation ? blocked(local_id_) : skip.contains(local_id_))) {
            Candidate c{.node_id = local_id_,
                        .is_local = true,
                        .cores_total = local_->CoreCapacity(),
                        .cores_free = self.cores_free,
                        .mem_available = self.mem_available,
                        .mem_total = self.mem_total,
                        .cpu_load_pct = self.cpu_load_pct,
                        .on_ac = self.on_ac,
                        .arch = self.arch,
                        .missing_bytes = 0};
            c.gpus = self.gpus;
            if (for_reservation) {
                c.mem_committed = shared_->MemInFlightLocked(local_id_);
                c.in_flight = shared_->InFlightLocked(local_id_);
                c.gpus_in_flight = shared_->GpusInFlightLocked(local_id_);
            }
            out.push_back(std::move(c));
        }
        const auto hazard = Hazard();
        const bool use_hazard = hazard && hazard->HasHistory();
        for (const auto& p : peers_->ListActive()) {
            if (p.id == local_id_ || (for_reservation ? blocked(p.id) : skip.contains(p.id))) continue;
            Candidate c{.node_id = p.id,
                        .is_local = false,
                        .cores_total = p.cores,
                        .cores_free = p.cores_free,
                        .mem_available = p.mem_available,
                        .mem_total = p.memory_bytes,
                        .core_limit = CoreLimitOf(p.id),
                        .cpu_load_pct = p.cpu_load_pct,
                        .on_ac = p.is_charging,
                        .arch = p.arch,
                        .latency = p.latency,
                        .missing_bytes = EstimatedMissingBytes(p.id, input_root_hash, input_tree_bytes),
                        .bandwidth_bytes_per_s = bandwidth_.Measured(p.id).value_or(0.0)};
            c.gpus = p.gpus;
            if (for_reservation) {
                c.mem_committed = shared_->MemInFlightLocked(p.id);
                c.in_flight = shared_->InFlightLocked(p.id);
                c.gpus_in_flight = shared_->GpusInFlightLocked(p.id);
            }
            if (use_hazard) c.expected_min_s = hazard->ExpectedMin(p.id, batch_s);
            out.push_back(std::move(c));
        }
        if (use_hazard) {
            for (auto& c : out) {
                if (c.is_local) c.expected_min_s = batch_s; // the requester outlives its own batches
            }
        }
        return out;
    };

    auto reserve_next = [&]() -> std::optional<Candidate> {
        std::lock_guard lock(shared_->mu);
        for (const Candidate& c : Rank(need, candidates(true), input_tree_bytes, batch_s)) {
            shared_->AddLocked(c.node_id, 1, mem, gpus);
            std::lock_guard block(batch->mu);
            batch->assigned_node = c.node_id;
            batch->is_local = c.is_local;
            return c;
        }
        return std::nullopt;
    };
    auto release = [&](const std::string& node_id) {
        std::lock_guard lock(shared_->mu);
        shared_->AddLocked(node_id, -1, mem, gpus);
        {
            std::lock_guard block(batch->mu);
            if (batch->assigned_node == node_id) batch->assigned_node.clear();
            batch->client.reset();
            batch->is_local = false;
            batch->running = false;
        }
        shared_->cv.notify_all();
    };
    // True when some node could take the batch if nothing were in flight: wait for capacity.
    auto any_eligible = [&] {
        std::lock_guard lock(shared_->mu);
        return !Rank(need, candidates(false), input_tree_bytes, batch_s).empty();
    };
    auto unplaceable = [&] {
        bool env = false, toolchain = false, reachable = false;
        std::string reasons;
        for (const auto& [node, why] : refused) {
            env = env || why.first == v1::REJECT_ENV;
            toolchain = toolchain || why.first == v1::REJECT_TOOLCHAIN;
            reachable = reachable || why.first != v1::REJECT_UNSPECIFIED;
            reasons += (reasons.empty() ? "" : "; ") + node.substr(0, 8) + ": " + why.second;
        }
        v1::FailureKind kind = v1::FAILURE_ENV_UNAVAILABLE;
        if (toolchain && !env) kind = v1::FAILURE_TOOLCHAIN_MISMATCH;
        if (!refused.empty() && !reachable) kind = v1::FAILURE_DONOR_LEFT;
        return Done(local_id_, kind, "no node can run this batch" + (reasons.empty() ? std::string() : ": " + reasons));
    };

    std::optional<std::vector<v1::Digest>> needed;
    for (;;) {
        if (batch->cancelled) return void(EndAttempt(batch, Done(local_id_, v1::FAILURE_CANCELLED, "cancelled", true)));
        auto reserved = reserve_next();
        if (!reserved) {
            // A hedge only takes a slot that is free now; it never queues behind real work.
            if (batch->is_hedge) return void(EndAttempt(batch, Done(local_id_, v1::FAILURE_CANCELLED, "no free slot", true)));
            if (!any_eligible()) return void(EndAttempt(batch, unplaceable()));
            tried.clear();
            std::unique_lock wait_lock(shared_->mu);
            shared_->cv.wait_for(wait_lock, kPlacementRecheck);
            continue;
        }
        const std::string node_id = reserved->node_id;
        tried.insert(node_id);
        if (batch->is_hedge && !batch->hedge_counted.exchange(true)) {
            std::lock_guard hedge_lock(hedge_mu_);
            ++hedge_stats_.launched;
        }

        if (reserved->is_local) {
            node::BatchDone d = RunLocal(batch);
            release(node_id);
            if (batch->cancelled) {
                d.failure = v1::FAILURE_CANCELLED;
                d.cancelled = true;
            }
            return void(EndAttempt(batch, std::move(d)));
        }

        auto peer = peers_->Get(node_id);
        auto client = peer ? factory_(*peer) : nullptr;
        if (!client) {
            release(node_id);
            skip.insert(node_id);
            refused[node_id] = {v1::REJECT_UNSPECIFIED, "not reachable"};
            continue;
        }
        if (!needed) {
            needed.emplace();
            std::unordered_set<std::string> seen;
            auto add = [&](const store::Digest& d) {
                if (!d.hash.empty() && seen.insert(d.hash).second) needed->push_back(node::ToProto(d));
            };
            for (const auto& d : merkle_->CollectTree(node::FromProto(spec.input_root()))) add(d);
            for (const auto& it : spec.items()) {
                if (it.checkpoint().hash().empty()) continue;
                for (const auto& d : merkle_->CollectTree(node::FromProto(it.checkpoint()))) add(d);
            }
        }
        {
            std::lock_guard lock(batch->mu);
            batch->client = client;
        }
        if (node::EncodedOfferByteSize(batch->id, spec, *needed) > static_cast<size_t>(node::kNodeApiMaxMessageSize)) {
            release(node_id);
            return void(EndAttempt(batch, Done(local_id_, v1::FAILURE_SANDBOX,
                                           "input tree too large to offer: " + std::to_string(needed->size()) + " digests")));
        }
        std::string err;
        auto reply = client->Offer(batch->id, spec, *needed, &err);
        if (!reply) {
            release(node_id);
            skip.insert(node_id);
            refused[node_id] = {v1::REJECT_UNSPECIFIED, err};
            continue;
        }
        NoteCoreLimit(node_id, *reply);
        if (!reply->accepted()) {
            release(node_id);
            const Rejection rej = ClassifyRejection(*reply);
            if (batch->cb.on_reject) batch->cb.on_reject(node_id, reply->reject_kind(), reply->reason());
            if (rej.passing) {
                defer_until[node_id] = std::chrono::steady_clock::now() + rej.retry_after;
            } else {
                skip.insert(node_id);
                refused[node_id] = {reply->reject_kind(), reply->reason()};
            }
            continue;
        }
        if (batch->cb.on_placed) batch->cb.on_placed(node_id);

        // Push what the donor lacks, letting it seed from other donors first.
        std::vector<v1::Digest> missing(reply->missing().begin(), reply->missing().end());
        {
            std::unordered_set<std::string> missing_hashes;
            for (const auto& d : missing) missing_hashes.insert(d.hash());
            std::vector<v1::Digest> held;
            for (const auto& d : *needed) {
                if (!missing_hashes.contains(d.hash())) held.push_back(d);
            }
            NoteHolders(node_id, held);
        }
        uint64_t wire_pushed = 0, seeded_before = 0;
        {
            std::lock_guard lock(traffic_mu_);
            seeded_before = traffic_[node_id].bytes_seeded;
        }
        bool pushed_ok = true;
        {
            auto donor = SentFor(node_id);
            std::lock_guard push_lock(donor->push_mu); // one push at a time per donor
            const auto now = std::chrono::steady_clock::now();
            std::vector<v1::Digest> to_send;
            {
                std::lock_guard lock(donor->mu);
                std::erase_if(donor->sent, [&](const auto& kv) { return now - kv.second > kSentTtl; });
                for (const auto& d : missing) {
                    if (!donor->sent.contains(d.hash())) to_send.push_back(d);
                }
            }
            const std::vector<v1::Digest> wanted = to_send;
            to_send = SeedFromPeers(node_id, *client, std::move(to_send));
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
            if (pushed_ok) {
                bandwidth_.Observe(node_id, wire_pushed, std::chrono::steady_clock::now() - push_start);
                std::lock_guard lock(donor->mu);
                for (const auto& d : to_send) donor->sent[d.hash()] = std::chrono::steady_clock::now();
            }
            if (pushed_ok) NoteHolders(node_id, to_send);
        }
        AddTraffic(node_id, wire_pushed, 0);
        if (!pushed_ok) {
            release(node_id);
            skip.insert(node_id);
            refused[node_id] = {v1::REJECT_UNSPECIFIED, "uploading inputs failed: " + err};
            continue;
        }
        {
            int64_t pushed = 0;
            for (const auto& d : missing) pushed += std::max<int64_t>(d.size_bytes(), 0);
            if (missing.empty()) pushed = input_tree_bytes;
            NoteInputsPushed(node_id, input_root_hash, pushed, input_tree_bytes);
        }

        node::BatchDone d = RunRemote(batch, node_id, client);
        release(node_id);
        d.bytes_pushed = wire_pushed;
        {
            std::lock_guard lock(traffic_mu_);
            d.bytes_seeded = traffic_[node_id].bytes_seeded - seeded_before;
        }
        return void(EndAttempt(batch, std::move(d)));
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

void Placer::HedgeScan() {
    HedgeConfig cfg;
    {
        std::lock_guard lock(hedge_mu_);
        cfg = hedge_cfg_;
    }
    if (!cfg.survival || cfg.max_copies < 2) return;

    struct Pick {
        std::shared_ptr<Batch> batch;
        std::string node;
        std::set<std::string> exclude;
    };
    std::vector<Pick> picks;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(shared_->mu);
        int unplaced = 0;
        int running = 0;
        for (auto& [id, b] : shared_->batches) {
            if (b->is_hedge || b->finished.load()) continue;
            std::lock_guard block(b->mu);
            if (b->assigned_node.empty()) {
                ++unplaced;
                continue;
            }
            ++running;
            if (b->is_local || !b->running || now < b->next_hedge) continue;
            int live = 0;
            bool pending = false; // a hedge was launched but has no donor slot yet
            std::set<std::string> exclude = b->exclude;
            exclude.insert(b->assigned_node);
            for (auto& h : b->hedges) {
                if (h->finished.load()) continue;
                ++live;
                std::lock_guard hlock(h->mu);
                if (h->assigned_node.empty() && !h->ever_ran) pending = true;
                if (!h->assigned_node.empty()) exclude.insert(h->assigned_node);
            }
            if (pending || live + 1 >= cfg.max_copies) continue;
            picks.push_back(Pick{b, b->assigned_node, std::move(exclude)});
        }
        // The tail: nothing is waiting for a slot and few batches are still running.
        if (unplaced > 0 || running > cfg.tail_max_running) return;
    }
    for (auto& pick : picks) {
        const double horizon = std::min(cfg.default_horizon_s, BatchSeconds(pick.batch->spec));
        const double p = cfg.survival(pick.node, horizon);
        if (!(p < cfg.survive_below)) continue; // also skips NaN
        LaunchHedge(pick.batch, std::move(pick.exclude), cfg);
    }
}

void Placer::LaunchHedge(const std::shared_ptr<Batch>& primary, std::set<std::string> exclude, const HedgeConfig& cfg) {
    bool target = false;
    for (const auto& p : peers_->ListActive()) {
        if (p.id != local_id_ && !exclude.contains(p.id)) target = true;
    }
    if (local_ && primary->spec.where() != v1::REMOTE_ONLY && !exclude.contains(local_id_)) target = true;
    if (!target) return;

    auto hedge = std::make_shared<Batch>();
    hedge->is_hedge = true;
    hedge->spec = primary->spec;
    hedge->exclude = std::move(exclude);
    {
        std::lock_guard lock(primary->mu);
        if (primary->finished.load()) return;
        hedge->id = primary->id + "#hedge" + std::to_string(++primary->hedge_seq);
        primary->next_hedge = std::chrono::steady_clock::now() + cfg.retry_after;
        primary->hedges.push_back(hedge);
    }
    // Items and checkpoints of the copy go to the primary's caller, which keeps the first of each.
    hedge->cb.on_started = primary->cb.on_started;
    hedge->cb.on_log = primary->cb.on_log;
    hedge->cb.on_item = primary->cb.on_item;
    hedge->cb.on_checkpoint = primary->cb.on_checkpoint;
    hedge->cb.on_done = [this, primary_weak = std::weak_ptr<Batch>(primary),
                         hedge_weak = std::weak_ptr<Batch>(hedge)](node::BatchDone d) {
        if (auto h = hedge_weak.lock()) OnHedgeDone(primary_weak.lock(), h, std::move(d));
    };
    {
        std::lock_guard lock(shared_->mu);
        shared_->batches[hedge->id] = hedge;
    }
    LaunchWorker(hedge);
}

void Placer::OnHedgeDone(const std::shared_ptr<Batch>& primary, const std::shared_ptr<Batch>& hedge, node::BatchDone d) {
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
    bool won = false;
    if (d.failure == v1::FAILURE_NONE && primary) {
        won = primary->Finish(d);
        if (won) Cancel(primary->id); // the original copy is no longer needed
    } else if (primary) {
        // This copy failed. If the original already failed and waits on its copies, and this was
        // the last one running, the original's failure stands.
        std::optional<node::BatchDone> deferred;
        {
            std::lock_guard lock(primary->mu);
            const bool others = std::any_of(primary->hedges.begin(), primary->hedges.end(),
                                            [](const auto& h) { return !h->finished.load(); });
            if (primary->deferred && !others) deferred = std::move(primary->deferred);
        }
        if (deferred) primary->Finish(std::move(*deferred));
    }
    const bool counted = hedge->hedge_counted.load();
    std::lock_guard lock(hedge_mu_);
    if (won) ++hedge_stats_.won;
    if (counted && !won && d.cancelled) ++hedge_stats_.cancelled;
    if (ran) {
        hedge_stats_.cpu_seconds += cpu;
        if (!won) hedge_stats_.wasted_cpu_seconds += cpu;
    }
}

void Placer::CancelHedges(const std::shared_ptr<Batch>& primary) {
    std::vector<std::shared_ptr<Batch>> hedges;
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
