#include "node/node_service.hpp"

#include "net/identity.hpp"
#include "net/tls.hpp"
#include "node/blob_transfer.hpp"
#include "node/convert.hpp"
#include "node/task_key.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <set>
#include <stdexcept>

namespace zeromesh::node {

namespace {

grpc::Status Denied(const std::string& why) { return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, why); }

grpc::Status NotConfigured(const char* what) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, std::string("this node does not serve ") + what);
}

grpc::Status Invalid(std::string why) { return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, std::move(why)); }

constexpr size_t kMaxTaskIdBytes = 256;
// A RunTask stream buffers at most this much log output before the task's log
// producer waits for the network, so a chatty task cannot bloat the daemon.
constexpr size_t kMaxQueuedLogBytes = 4 * 1024 * 1024;

// The terminal state for a result, as JobManager judges it.
std::pair<v1::TaskState, std::string> FinalState(const v1::TaskResult& r) {
    if (r.cancelled()) return {v1::CANCELLED, "cancelled"};
    if (!r.error().empty()) return {v1::FAILED, r.error()};
    if (r.timed_out()) return {v1::FAILED, "timed out"};
    if (r.exit_code() != 0) return {v1::FAILED, "exit code " + std::to_string(r.exit_code())};
    return {v1::SUCCEEDED, ""};
}

// Events of one RunTask call, from the executor's threads to the RPC thread.
struct RunStream {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<v1::JobEvent> events;
    size_t queued_log_bytes{0};
    bool done{false};    // the final event is queued
    bool closed{false};  // the caller is gone: drop logs, never wait

    void Push(v1::JobEvent ev, bool final_event) {
        std::lock_guard lock(mu);
        events.push_back(std::move(ev));
        if (final_event) done = true;
        cv.notify_all();
    }

    void PushLog(int fd, std::string_view chunk) {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return closed || queued_log_bytes < kMaxQueuedLogBytes; });
        if (closed) return;
        v1::JobEvent ev;
        ev.set_log_fd(fd);
        ev.set_log_chunk(std::string(chunk));
        queued_log_bytes += chunk.size();
        events.push_back(std::move(ev));
        cv.notify_all();
    }

    void Close() {
        std::lock_guard lock(mu);
        closed = true;
        cv.notify_all();
    }
};

std::string MacBytes(const std::array<uint8_t, 32>& mac) { return std::string(mac.begin(), mac.end()); }

} // namespace

NodeService::NodeService(std::string node_id, std::shared_ptr<net::PinStore> pins,
                         std::function<std::optional<net::TeamKey>()> team, std::string self_fingerprint,
                         std::function<v1::PeerInfo()> self_info, std::shared_ptr<store::CasStore> cas,
                         std::shared_ptr<plugin::Registry> registry, std::shared_ptr<LocalExecutor> executor,
                         std::function<net::Telemetry()> telemetry, DonorPolicy policy,
                         std::shared_ptr<net::PeerTable> peers)
    : node_id_(std::move(node_id)),
      pins_(std::move(pins)),
      team_(std::move(team)),
      self_fp_(std::move(self_fingerprint)),
      self_info_(std::move(self_info)),
      cas_(std::move(cas)),
      registry_(std::move(registry)),
      executor_(std::move(executor)),
      telemetry_(std::move(telemetry)),
      policy_(policy),
      peers_(std::move(peers)) {
    if (!pins_ || !team_ || !self_info_) throw std::invalid_argument("NodeService: missing dependency");
    if (!net::IsFingerprint(self_fp_)) throw std::invalid_argument("NodeService: bad self fingerprint");
}

NodeService::~NodeService() = default;

bool NodeService::RemoteExecConfigured() const { return cas_ && registry_ && executor_ && telemetry_; }

void NodeService::PurgeExpiredLocked() {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(reserved_, [&](const auto& kv) { return kv.second.expires <= now; });
}

int NodeService::CoresInUseLocked() const {
    int n = 0;
    for (const auto& [key, r] : reserved_) n += std::max(r.spec.resources().cores(), 1);
    for (const auto& [key, r] : running_) n += r.cores;
    return n;
}

grpc::Status NodeService::RequirePinned(const grpc::ServerContext& ctx, std::string* peer_node_id) const {
    std::string fp = net::PeerFingerprint(ctx);
    if (fp.empty()) return Denied("no client certificate");
    auto node = pins_->NodeFor(fp);
    if (!node) return Denied("peer is not admitted to this team");
    if (peer_node_id) *peer_node_id = std::move(*node);
    return grpc::Status::OK;
}

grpc::Status NodeService::Admit(grpc::ServerContext* ctx, const v1::AdmitHello* req, v1::AdmitChallenge* resp) {
    auto key = team_();
    if (!key) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "node is not in a team");
    std::string fp_client = net::PeerFingerprint(*ctx);
    if (fp_client.empty()) return Denied("no client certificate");
    if (!net::IsValidNodeId(req->node_id())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid node id");
    }
    if (req->nonce().size() != kAdmitNonceBytes) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "nonce must be 32 bytes");
    }

    std::string nonce_s = AdmitNonce();
    resp->set_node_id(node_id_);
    resp->set_nonce(nonce_s);
    resp->set_mac(MacBytes(AdmitServerMac(*key, req->nonce(), self_fp_, fp_client)));
    pending_.Begin(fp_client, req->node_id(), std::move(nonce_s));
    return grpc::Status::OK;
}

grpc::Status NodeService::AdmitConfirm(grpc::ServerContext* ctx, const v1::AdmitProof* req, v1::Empty* /*resp*/) {
    std::string fp_client = net::PeerFingerprint(*ctx);
    if (fp_client.empty()) return Denied("no client certificate");
    auto entry = pending_.Take(fp_client); // single use, MAC match or not
    if (!entry) return Denied("no pending admission for this certificate");
    auto key = team_();
    if (!key) return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "node is not in a team");
    if (!AdmitMacMatches(req->mac(), AdmitClientMac(*key, entry->nonce_s, fp_client, self_fp_))) {
        return Denied("admission proof does not match this team");
    }
    try {
        pins_->Pin(entry->node_id, fp_client);
    } catch (const std::exception& e) {
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("cannot pin peer: ") + e.what());
    }
    return grpc::Status::OK;
}

grpc::Status NodeService::Ping(grpc::ServerContext* ctx, const v1::Empty* /*req*/, v1::PeerInfo* resp) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    *resp = self_info_();
    return grpc::Status::OK;
}

grpc::Status NodeService::OfferTask(grpc::ServerContext* ctx, const v1::Offer* req, v1::OfferReply* resp) {
    std::string peer;
    if (auto st = RequirePinned(*ctx, &peer); !st.ok()) return st;
    if (!RemoteExecConfigured()) return NotConfigured("remote tasks");
    if (req->task_id().empty() || req->task_id().size() > kMaxTaskIdBytes) return Invalid("invalid task id");

    v1::TaskSpec spec = req->spec();
    Normalize(spec);
    if (spec.argv().empty()) return Invalid("empty argv");
    const v1::Digest& root = spec.input_root();
    if (!root.hash().empty() && !store::CasStore::IsValidHash(root.hash())) return Invalid("invalid input_root digest");

    // Everything the run needs: what the caller listed plus the input root itself.
    std::vector<store::Digest> needed;
    std::set<std::string> seen;
    auto add = [&](const v1::Digest& d) {
        if (seen.insert(d.hash()).second) needed.push_back(FromProto(d));
    };
    for (const auto& d : req->needed()) {
        if (!store::CasStore::IsValidHash(d.hash()) || d.size_bytes() < 0) return Invalid("invalid digest in needed");
        add(d);
    }
    if (!root.hash().empty()) add(root);

    auto reject = [&](std::string reason) {
        resp->set_accepted(false);
        resp->set_reason(std::move(reason));
        return grpc::Status::OK;
    };

    const std::string& env_name = spec.env_spec().name();
    plugin::IEnvProvider* env = registry_->Env(env_name);
    if (env == nullptr) return reject("no env provider '" + env_name + "' on this node");
    plugin::EnvParams params;
    params.params.insert(spec.env_spec().params().begin(), spec.env_spec().params().end());
    params.argv.assign(spec.argv().begin(), spec.argv().end());
    try {
        plugin::Availability avail = env->Check(params);
        if (!avail.ok) {
            return reject(avail.reason.empty() ? "env provider '" + env_name + "' cannot run this task" : avail.reason);
        }
    } catch (const std::exception& e) {
        return reject("env provider '" + env_name + "': " + e.what());
    }

    const net::Telemetry telemetry = telemetry_();
    std::vector<v1::Digest> missing;
    for (const auto& d : needed) {
        if (!cas_->HasBlob(d.hash)) missing.push_back(ToProto(d));
    }

    std::lock_guard lock(mu_);
    PurgeExpiredLocked();
    const TaskKey key{peer, req->task_id()};
    if (running_.contains(key)) return reject("task " + req->task_id() + " is already running here");
    reserved_.erase(key);  // a repeated offer replaces the old reservation
    if (auto why = policy_.Refuses(telemetry, spec, CoresInUseLocked())) return reject(*why);

    resp->set_accepted(true);
    for (auto& d : missing) *resp->add_missing() = std::move(d);
    reserved_[key] = Reservation{std::move(spec), std::move(needed),
                                 std::chrono::steady_clock::now() + kOfferReservation};
    return grpc::Status::OK;
}

grpc::Status NodeService::PushBlobs(grpc::ServerContext* ctx, grpc::ServerReader<v1::Chunk>* reader,
                                    v1::Empty* /*resp*/) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    if (!cas_) return NotConfigured("blobs");
    BlobReceiver receiver(*cas_);
    v1::Chunk chunk;
    while (reader->Read(&chunk)) {
        if (std::string err = receiver.Feed(chunk); !err.empty()) return Invalid(std::move(err));
    }
    if (std::string err = receiver.Finish(); !err.empty()) return Invalid(std::move(err));
    return grpc::Status::OK;
}

grpc::Status NodeService::PullBlobs(grpc::ServerContext* ctx, const v1::Digests* req,
                                    grpc::ServerWriter<v1::Chunk>* writer) {
    if (auto st = RequirePinned(*ctx); !st.ok()) return st;
    if (!cas_) return NotConfigured("blobs");
    for (const auto& d : req->digests()) {
        if (!store::CasStore::IsValidHash(d.hash())) return Invalid("invalid digest hash");
        if (!cas_->HasBlob(d.hash())) return grpc::Status(grpc::StatusCode::NOT_FOUND, "blob " + d.hash() + " not found");
    }
    auto write = [&](const v1::Chunk& c) { return !ctx->IsCancelled() && writer->Write(c); };
    for (const auto& d : req->digests()) {
        if (std::string err = SendBlob(*cas_, FromProto(d), write); !err.empty()) {
            if (ctx->IsCancelled()) return grpc::Status(grpc::StatusCode::CANCELLED, "caller went away");
            return grpc::Status(grpc::StatusCode::INTERNAL, err);
        }
    }
    return grpc::Status::OK;
}

grpc::Status NodeService::RunTask(grpc::ServerContext* ctx, const v1::RunRequest* req,
                                  grpc::ServerWriter<v1::JobEvent>* writer) {
    std::string peer;
    if (auto st = RequirePinned(*ctx, &peer); !st.ok()) return st;
    if (!RemoteExecConfigured()) return NotConfigured("remote tasks");
    const TaskKey key{peer, req->task_id()};
    auto no_reservation = [&] {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "no reservation for task " + req->task_id() + " (offer it first; offers expire after 30 s)");
    };

    std::vector<store::Digest> needed;
    {
        std::lock_guard lock(mu_);
        PurgeExpiredLocked();
        auto it = reserved_.find(key);
        if (it == reserved_.end()) return no_reservation();
        needed = it->second.needed;
    }
    size_t absent = 0;
    std::string first_absent;
    for (const auto& d : needed) {
        if (!cas_->HasBlob(d.hash)) {
            if (absent++ == 0) first_absent = d.hash;
        }
    }
    if (absent > 0) {
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            std::to_string(absent) + " input blobs were not pushed (first: " + first_absent + ")");
    }

    v1::TaskSpec spec;
    const std::string exec_id = peer + "/" + req->task_id();
    {
        std::lock_guard lock(mu_);
        PurgeExpiredLocked();
        auto it = reserved_.find(key);
        if (it == reserved_.end()) return no_reservation();
        spec = std::move(it->second.spec);
        reserved_.erase(it);
        running_[key] = Running{exec_id, std::max(spec.resources().cores(), 1)};
    }

    auto stream = std::make_shared<RunStream>();
    const std::string task_name = spec.name();
    TaskCallbacks cb;
    cb.on_state = [stream, task_name](v1::TaskState s, std::string detail) {
        v1::JobEvent ev;
        ev.set_task_name(task_name);
        ev.set_state(s);
        ev.set_detail(std::move(detail));
        stream->Push(std::move(ev), false);
    };
    cb.on_log = [stream](int fd, std::string_view chunk) { stream->PushLog(fd, chunk); };
    cb.on_done = [stream, task_name](v1::TaskResult result) {
        v1::JobEvent ev;
        ev.set_task_name(task_name);
        auto [state, detail] = FinalState(result);
        ev.set_state(state);
        ev.set_detail(std::move(detail));
        *ev.mutable_result() = std::move(result);
        stream->Push(std::move(ev), true);
    };
    executor_->Execute(exec_id, spec, std::move(cb));

    // Forward events until the final one; on a vanished caller, cancel the task and
    // still wait for its single on_done so nothing outlives this call.
    bool cancel_sent = false;
    bool caller_gone = false;
    for (bool finished = false; !finished;) {
        std::deque<v1::JobEvent> batch;
        {
            std::unique_lock lock(stream->mu);
            stream->cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return !stream->events.empty(); });
            batch.swap(stream->events);
            stream->queued_log_bytes = 0;
            finished = stream->done && stream->events.empty();
            stream->cv.notify_all();
        }
        if (!caller_gone && ctx->IsCancelled()) caller_gone = true;
        for (auto& ev : batch) {
            if (!caller_gone && !writer->Write(ev)) caller_gone = true;
        }
        if (caller_gone && !cancel_sent) {
            cancel_sent = true;
            stream->Close();
            executor_->Cancel(exec_id);
        }
    }

    {
        std::lock_guard lock(mu_);
        running_.erase(key);
    }
    if (caller_gone) return grpc::Status(grpc::StatusCode::CANCELLED, "caller went away");
    return grpc::Status::OK;
}

grpc::Status NodeService::CancelTask(grpc::ServerContext* ctx, const v1::TaskRef* req, v1::Empty* /*resp*/) {
    std::string peer;
    if (auto st = RequirePinned(*ctx, &peer); !st.ok()) return st;
    if (!RemoteExecConfigured()) return NotConfigured("remote tasks");
    // Only the node that offered a task can cancel it: the key includes the caller's id.
    const TaskKey key{peer, req->task_id()};
    std::string exec_id;
    {
        std::lock_guard lock(mu_);
        reserved_.erase(key);
        auto it = running_.find(key);
        if (it != running_.end()) exec_id = it->second.exec_id;
    }
    if (!exec_id.empty()) executor_->Cancel(exec_id);
    return grpc::Status::OK;
}

grpc::Status NodeService::Drain(grpc::ServerContext* ctx, const v1::DrainNotice* req, v1::Empty* /*resp*/) {
    std::string peer;
    if (auto st = RequirePinned(*ctx, &peer); !st.ok()) return st;
    if (!peers_) return NotConfigured("drain notices");
    // The sender is the authenticated caller, whatever node id the notice names.
    peers_->MarkDraining(peer, req->reason());
    return grpc::Status::OK;
}

} // namespace zeromesh::node
