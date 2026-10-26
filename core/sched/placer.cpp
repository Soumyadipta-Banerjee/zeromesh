#include "sched/placer.hpp"

#include "node/convert.hpp"
#include "sched/rank.hpp"

#include <atomic>
#include <exception>
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

} // namespace

struct Placer::Task {
    std::string id;
    v1::TaskSpec spec;
    node::TaskCallbacks cb;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> finished{false};  // on_done was called
    std::atomic<bool> worker_done{false};

    std::mutex mu;
    bool is_local{false};
    std::shared_ptr<node::NodeClient> client;  // set while placing on / running at a peer

    bool IsLocal() {
        std::lock_guard lock(mu);
        return is_local;
    }

    void Done(v1::TaskResult r) {
        if (finished.exchange(true)) return;
        if (cb.on_done) cb.on_done(std::move(r));
    }
};

// Shared with callbacks that may outlive the Placer (the LocalExecutor's).
struct Placer::Shared {
    std::mutex mu;
    std::map<std::string, std::shared_ptr<Task>> tasks;
    std::map<std::string, int> in_flight;  // node id -> tasks we run there

    int InFlight(const std::string& node) {
        std::lock_guard lock(mu);
        auto it = in_flight.find(node);
        return it == in_flight.end() ? 0 : it->second;
    }
    void Add(const std::string& node, int delta) {
        std::lock_guard lock(mu);
        if ((in_flight[node] += delta) <= 0) in_flight.erase(node);
    }
    void Forget(const std::string& task_id) {
        std::lock_guard lock(mu);
        tasks.erase(task_id);
    }
};

Placer::Placer(std::shared_ptr<node::LocalExecutor> local, std::shared_ptr<net::PeerTable> peers,
               ClientFactory factory, std::shared_ptr<store::CasStore> cas,
               std::shared_ptr<store::MerkleBuilder> merkle, std::string local_node_id,
               std::function<net::Telemetry()> telemetry)
    : local_(std::move(local)),
      peers_(std::move(peers)),
      factory_(std::move(factory)),
      cas_(std::move(cas)),
      merkle_(std::move(merkle)),
      local_id_(std::move(local_node_id)),
      telemetry_(std::move(telemetry)),
      shared_(std::make_shared<Shared>()) {
    if (!peers_ || !factory_ || !cas_ || !merkle_) throw std::invalid_argument("Placer: missing dependency");
}

Placer::~Placer() {
    std::vector<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard lock(shared_->mu);
        for (auto& [id, t] : shared_->tasks) tasks.push_back(t);
    }
    for (auto& t : tasks) {
        if (!t->IsLocal()) Cancel(t->id);  // local tasks belong to the LocalExecutor
    }
    std::lock_guard lock(workers_mu_);
    workers_.clear();  // joins
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

void Placer::ReapFinishedLocked() {
    std::erase_if(workers_, [](const Worker& w) { return w.task->worker_done.load(); });
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
                                  if (!task->IsLocal()) shared_->Forget(task->id);
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
        client->Cancel(task_id);  // a no-op unless its Run is in progress
    }
}

void Placer::RunLocal(const std::shared_ptr<Task>& task, const v1::TaskSpec& spec) {
    {
        std::lock_guard lock(task->mu);
        task->is_local = true;
    }
    shared_->Add(local_id_, 1);
    node::TaskCallbacks cb;
    cb.on_state = task->cb.on_state;
    cb.on_log = task->cb.on_log;
    cb.on_done = [shared = shared_, task, node = local_id_](v1::TaskResult r) {
        shared->Add(node, -1);
        shared->Forget(task->id);
        task->Done(std::move(r));
    };
    local_->Execute(task->id, spec, std::move(cb));
    if (task->cancelled) local_->Cancel(task->id);  // raced with Cancel
}

namespace {

// Pulls stdout/stderr and every output (whole trees for directories) that the local CAS lacks.
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
    // Level by level: fetch the manifests, then their files and the next level's manifests.
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

void Placer::Place(const std::shared_ptr<Task>& task) {
    v1::TaskSpec spec = task->spec;
    const net::Telemetry self = telemetry_ ? telemetry_() : net::Telemetry{};
    if (spec.resources().arch().empty() && !self.arch.empty()) spec.mutable_resources()->set_arch(self.arch);

    std::vector<Candidate> candidates;
    if (local_) {
        candidates.push_back(Candidate{.node_id = local_id_,
                                       .is_local = true,
                                       .cores_free = self.cores_free,
                                       .mem_available = self.mem_available,
                                       .cpu_load_pct = self.cpu_load_pct,
                                       .on_ac = self.on_ac,
                                       .arch = self.arch,
                                       .latency = std::chrono::milliseconds(0),
                                       .missing_bytes = 0,
                                       .in_flight = shared_->InFlight(local_id_)});
    }
    for (const auto& p : peers_->ListActive()) {
        if (p.id == local_id_) continue;
        candidates.push_back(Candidate{.node_id = p.id,
                                       .is_local = false,
                                       .cores_free = p.cores_free,
                                       .mem_available = p.mem_available,
                                       .cpu_load_pct = p.cpu_load_pct,
                                       .on_ac = p.is_charging,
                                       .arch = p.arch,
                                       .latency = p.latency,
                                       .missing_bytes = -1,
                                       .in_flight = shared_->InFlight(p.id)});
    }

    std::string last_reason = "no eligible nodes";
    std::optional<std::vector<v1::Digest>> needed;
    for (const Candidate& c : Rank(spec, std::move(candidates))) {
        if (task->cancelled) return task->Done(Cancelled(local_id_));
        if (c.is_local) return RunLocal(task, spec);

        auto peer = peers_->Get(c.node_id);
        auto client = peer ? factory_(*peer) : nullptr;
        if (!client) {
            last_reason = "node " + c.node_id + ": not reachable";
            continue;
        }
        if (!needed) {
            needed.emplace();
            for (const auto& d : merkle_->CollectTree(node::FromProto(spec.input_root()))) {
                needed->push_back(node::ToProto(d));
            }
        }
        {
            std::lock_guard lock(task->mu);
            task->client = client;
        }

        std::string err;
        auto reply = client->Offer(task->id, spec, *needed, &err);
        if (!reply) {
            last_reason = "node " + c.node_id + ": " + err;
            continue;
        }
        if (!reply->accepted()) {
            last_reason = "node " + c.node_id + ": " + reply->reason();
            continue;
        }
        if (task->cb.on_state) task->cb.on_state(v1::PLACED, "node " + c.node_id);

        std::vector<v1::Digest> missing(reply->missing().begin(), reply->missing().end());
        if (!client->Push(*cas_, missing, &err)) {
            last_reason = "node " + c.node_id + ": uploading inputs failed: " + err;
            continue;
        }
        if (task->cancelled) return task->Done(Cancelled(local_id_));

        shared_->Add(c.node_id, 1);
        bool cancel_forwarded = false;
        auto on_event = [&](const v1::JobEvent& ev) {
            if (task->cancelled && !cancel_forwarded) {
                cancel_forwarded = true;  // Cancel() arrived before the stream was registered
                client->Cancel(task->id);
            }
            if (ev.has_result()) return;  // the final event: returned by Run
            if (ev.log_fd() != 0) {
                if (task->cb.on_log) task->cb.on_log(ev.log_fd(), ev.log_chunk());
            } else if ((ev.state() == v1::PLACED || ev.state() == v1::RUNNING) && task->cb.on_state) {
                task->cb.on_state(ev.state(), ev.detail());
            }
        };
        auto result = client->Run(task->id, on_event, &err);
        shared_->Add(c.node_id, -1);
        if (!result) {
            if (task->cancelled) return task->Done(Cancelled(c.node_id));
            return task->Done(Failed(c.node_id, "node " + c.node_id + " lost during run (" + err + ")"));
        }
        // The pinned channel authenticated the peer; record that identity, not its claim.
        result->set_executed_by(c.node_id);
        if (!result->cancelled() && !PullOutputs(*client, *cas_, *result, &err)) {
            return task->Done(Failed(c.node_id, "node " + c.node_id + " lost while sending outputs: " + err));
        }
        return task->Done(std::move(*result));
    }
    task->Done(Failed(local_id_, "no node can run this task: " + last_reason));
}

} // namespace zeromesh::sched
