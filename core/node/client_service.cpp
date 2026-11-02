#include "node/client_service.hpp"

#include "node/convert.hpp"
#include "store/deny_list.hpp"
#include "store/stat_cache.hpp"

#include <fcntl.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace zeromesh::node {

namespace {

namespace fs = std::filesystem;

grpc::Status Invalid(const std::string& msg) {
    return {grpc::StatusCode::INVALID_ARGUMENT, msg};
}

std::string HumanBytes(uint64_t n) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    size_t u = 0;
    while (v >= 1024.0 && u + 1 < std::size(kUnits)) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, kUnits[u]);
    return buf;
}

// Sums the sizes of the regular files BuildTree would store, without following symlinks.
// Returns false as soon as the sum exceeds `limit`.
bool SizeWithinLimit(const fs::path& dir, const std::string& rel, const store::IgnoreRules& rules, uint64_t limit,
                     uint64_t& total) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::string child_rel = rel.empty() ? name : rel + "/" + name;
        struct stat st{};
        if (lstat(it->path().c_str(), &st) != 0) continue;
        const bool is_dir = S_ISDIR(st.st_mode);
        if (rules.Ignored(child_rel, is_dir)) continue;
        if (is_dir) {
            if (!SizeWithinLimit(it->path(), child_rel, rules, limit, total)) return false;
        } else if (S_ISREG(st.st_mode)) {
            total += static_cast<uint64_t>(st.st_size);
            if (total > limit) return false;
        }
    }
    return true;
}

v1::PeerInfo FromPeer(const net::Peer& p) {
    v1::PeerInfo info;
    info.set_node_id(p.id);
    info.set_hostname(p.hostname);
    info.set_address(p.ip_address);
    info.set_node_port(p.node_port);
    info.set_arch(p.arch);
    info.set_cores_total(p.cores);
    info.set_cores_free(p.cores_free);
    info.set_mem_available(static_cast<int64_t>(p.mem_available));
    info.set_cpu_load_pct(p.cpu_load_pct);
    info.set_battery_pct(p.battery_pct);
    info.set_on_ac(p.is_charging);
    info.set_mem_total(static_cast<int64_t>(p.memory_bytes));
    info.set_work_mem(static_cast<int64_t>(p.work_memory_bytes));
    info.set_gpus(p.gpus);
    switch (p.state) {
    case net::PeerState::Active:
        info.set_state("active");
        break;
    case net::PeerState::Draining:
        info.set_state("draining");
        break;
    case net::PeerState::Offline:
        info.set_state("offline");
        break;
    }
    return info;
}

} // namespace

uint64_t UploadLimitFromEnv() {
    const char* v = std::getenv("ZM_UPLOAD_LIMIT_BYTES");
    if (v == nullptr || *v == '\0') return kDefaultUploadLimitBytes;
    char* end = nullptr;
    errno = 0;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (errno != 0 || end == v || *end != '\0' || n == 0) return kDefaultUploadLimitBytes;
    return n;
}

std::optional<net::TeamKey> TeamState::Get() {
    std::lock_guard lock(mu);
    return key;
}

void TeamState::Apply(std::optional<net::TeamKey> next) {
    {
        std::lock_guard lock(mu);
        key = next;
    }
    if (pins) pins->Clear();
    if (next) {
        net::TeamFile::Save(state_dir, *next);
    } else {
        net::TeamFile::Remove(state_dir);
    }
    if (discovery) discovery->SetTeam(next);
    if (on_changed) on_changed();
}

ClientService::ClientService(std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
                             std::shared_ptr<net::PeerTable> peers, ClientServiceOptions options, ClientServiceParts parts,
                             std::shared_ptr<TeamState> team)
    : cas_(std::move(cas)), merkle_(std::move(merkle)), peers_(std::move(peers)), options_(std::move(options)),
      parts_(std::move(parts)), team_(std::move(team)) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium initialisation failed");
    telemetry_.Sample(); // prime the CPU-load delta
}

grpc::Status ClientService::PutObjects(grpc::ServerContext*, grpc::ServerReader<v1::Chunk>* reader,
                                       v1::PutObjectsReply* reply) {
    std::optional<store::Digest> cur;
    std::string buf;
    v1::Chunk chunk;
    while (reader->Read(&chunk)) {
        if (chunk.has_digest()) {
            if (cur) return Invalid("a new object started before the previous one ended (missing last=true)");
            if (!store::CasStore::IsValidHash(chunk.digest().hash()) || chunk.digest().size_bytes() < 0) {
                return Invalid("invalid digest '" + chunk.digest().hash() + "'");
            }
            cur = FromProto(chunk.digest());
            buf.clear();
        } else if (!cur) {
            return Invalid("the first chunk of each object must carry its digest");
        }
        buf += chunk.data();
        if (static_cast<int64_t>(buf.size()) > cur->size_bytes) {
            return Invalid("object " + cur->hash + " is larger than its declared size");
        }
        if (chunk.last()) {
            store::Digest got = store::CasStore::ComputeDigest(buf);
            if (!(got == *cur)) {
                return Invalid("digest mismatch: declared " + cur->hash + "/" + std::to_string(cur->size_bytes) +
                               ", received " + got.hash + "/" + std::to_string(got.size_bytes));
            }
            cas_->PutBlob(buf);
            *reply->add_stored() = ToProto(got);
            cur.reset();
            buf.clear();
        }
    }
    if (cur) return Invalid("stream ended in the middle of object " + cur->hash);
    return grpc::Status::OK;
}

grpc::Status ClientService::GetObjects(grpc::ServerContext* ctx, const v1::Digests* req,
                                       grpc::ServerWriter<v1::Chunk>* writer) {
    for (const auto& d : req->digests()) {
        if (!store::CasStore::IsValidHash(d.hash())) return Invalid("invalid digest '" + d.hash() + "'");
        if (!cas_->HasBlob(d.hash())) return {grpc::StatusCode::NOT_FOUND, "object " + d.hash() + " not found"};
    }
    std::vector<char> buf(kObjectChunkBytes);
    for (const auto& d : req->digests()) {
        std::string data;
        std::ifstream in(cas_->GetBlobPath(d.hash()), std::ios::binary);
        if (!in.is_open()) {
            // Small blobs may live only in the in-memory cache.
            auto blob = cas_->GetBlob(d.hash());
            if (!blob) return {grpc::StatusCode::NOT_FOUND, "object " + d.hash() + " not found"};
            data = std::move(*blob);
        }
        bool first = true;
        size_t mem_off = 0;
        for (;;) {
            if (ctx->IsCancelled()) return grpc::Status::CANCELLED;
            v1::Chunk c;
            if (first) *c.mutable_digest() = d;
            first = false;
            if (in.is_open()) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                c.set_data(buf.data(), static_cast<size_t>(in.gcount()));
                c.set_last(in.eof() || in.gcount() == 0);
                if (in.bad()) return {grpc::StatusCode::INTERNAL, "cannot read object " + d.hash()};
            } else {
                size_t n = std::min(kObjectChunkBytes, data.size() - mem_off);
                c.set_data(data.data() + mem_off, n);
                mem_off += n;
                c.set_last(mem_off >= data.size());
            }
            bool last = c.last();
            if (!writer->Write(c)) return grpc::Status::CANCELLED;
            if (last) break;
        }
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::FindMissing(grpc::ServerContext*, const v1::Digests* req, v1::Digests* reply) {
    for (const auto& d : req->digests()) {
        if (!cas_->HasBlob(d.hash())) *reply->add_digests() = d;
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::Snapshot(grpc::ServerContext*, const v1::SnapshotRequest* req, v1::SnapshotReply* reply) {
    const fs::path root = req->path();
    if (!root.is_absolute()) return Invalid("snapshot path must be absolute: '" + req->path() + "'");
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return Invalid("snapshot path is not a directory: " + root.string());

    std::vector<std::string> exclude(req->exclude().begin(), req->exclude().end());
    // This daemon's own state (its store above all) never goes into a snapshot that contains it:
    // it would ship the store, and grow it by itself on every snapshot.
    if (!options_.state_dir.empty()) {
        const fs::path state = fs::weakly_canonical(options_.state_dir, ec);
        const fs::path rel = state.lexically_relative(fs::weakly_canonical(root, ec));
        if (!rel.empty() && *rel.begin() != ".." && rel != ".") exclude.push_back("/" + rel.generic_string() + "/");
    }
    const store::IgnoreRules ignore = store::IgnoreRules::ForRoot(root, exclude);
    const store::IgnoreRules deny = store::DenyRules();
    if (!req->force_large()) {
        uint64_t total = 0;
        if (!SizeWithinLimit(root, "", ignore, options_.upload_limit_bytes, total)) {
            return {grpc::StatusCode::RESOURCE_EXHAUSTED,
                    "refusing to snapshot " + root.string() + ": more than " + HumanBytes(options_.upload_limit_bytes) +
                        " after .zmignore; add paths to .zmignore or pass --force-large"};
        }
    }
    std::lock_guard lock(snapshot_mu_);
    const fs::path cache_file =
        options_.state_dir.empty() ? fs::path()
                                   : options_.state_dir / "statcache" / store::CasStore::ComputeDigest(root.string()).hash;
    store::StatCache stat_cache(cache_file);
    std::vector<std::string> denied;
    uint64_t total = 0, files = 0;
    store::TreeOptions opts;
    opts.ignore = &ignore;
    opts.deny = &deny;
    opts.denied = &denied;
    opts.stat_cache = &stat_cache;
    opts.total_bytes = &total;
    opts.file_count = &files;
    try {
        *reply->mutable_root() = ToProto(merkle_->BuildTree(root, opts));
    } catch (const std::exception& e) {
        return Invalid(std::string("cannot snapshot ") + root.string() + ": " + e.what());
    }
    stat_cache.Save();
    reply->set_total_bytes(static_cast<int64_t>(total));
    reply->set_files(static_cast<int64_t>(files));
    reply->set_rehashed(static_cast<int64_t>(stat_cache.misses()));
    std::sort(denied.begin(), denied.end());
    for (const auto& d : denied) reply->add_denied(d);
    if (total > kUploadWarnBytes) {
        reply->set_warning("the workspace is " + HumanBytes(total) + "; consider a .zmignore");
    }
    if (req->list()) {
        // The biggest files, for --show-upload.
        std::vector<std::pair<int64_t, std::string>> sizes;
        for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            const std::string rel = fs::relative(it->path(), root, ec).string();
            const bool dir = it->is_directory(ec) && !it->is_symlink(ec);
            if (ignore.Ignored(rel, dir) || deny.Ignored(rel, dir)) {
                if (dir) it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(ec) && !it->is_symlink(ec)) {
                sizes.emplace_back(static_cast<int64_t>(it->file_size(ec)), rel);
            }
        }
        std::sort(sizes.rbegin(), sizes.rend());
        for (size_t i = 0; i < sizes.size() && i < 20; ++i) {
            auto* f = reply->add_largest();
            f->set_path(sizes[i].second);
            f->set_bytes(sizes[i].first);
        }
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::CaptureEnv(grpc::ServerContext*, const v1::CaptureRequest* req, v1::CaptureReply* reply) {
    if (!parts_.capture) return {grpc::StatusCode::UNIMPLEMENTED, "environment capture is not available"};
    return parts_.capture(*req, reply);
}

namespace {

grpc::Status NoEngine() {
    return {grpc::StatusCode::UNIMPLEMENTED, "runs are not available on this daemon"};
}

grpc::Status UnknownRun(const std::string& id) {
    return {grpc::StatusCode::NOT_FOUND, "unknown run '" + id + "'"};
}

} // namespace

grpc::Status ClientService::SubmitRun(grpc::ServerContext*, const v1::RunSpec* req, v1::RunRef* reply) {
    if (!parts_.engine) return NoEngine();
    try {
        reply->set_run_id(parts_.engine->Submit(*req));
    } catch (const std::invalid_argument& e) {
        return Invalid(e.what());
    } catch (const std::exception& e) {
        return {grpc::StatusCode::INTERNAL, e.what()};
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::AddItems(grpc::ServerContext*, const v1::AddItemsRequest* req, v1::Empty*) {
    if (!parts_.engine) return NoEngine();
    try {
        parts_.engine->AddItems(req->run_id(), {req->items().begin(), req->items().end()});
    } catch (const std::invalid_argument& e) {
        return Invalid(e.what());
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::CloseRun(grpc::ServerContext*, const v1::RunRef* req, v1::Empty*) {
    if (!parts_.engine) return NoEngine();
    try {
        parts_.engine->Close(req->run_id());
    } catch (const std::invalid_argument& e) {
        return UnknownRun(req->run_id());
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::WatchRun(grpc::ServerContext* ctx, const v1::WatchRequest* req,
                                     grpc::ServerWriter<v1::RunEvent>* writer) {
    if (!parts_.engine) return NoEngine();
    auto stream = parts_.engine->Watch(req->run_id(), req->from_start());
    if (!stream) return UnknownRun(req->run_id());
    for (;;) {
        if (ctx->IsCancelled()) {
            stream->Close();
            return grpc::Status::CANCELLED;
        }
        if (auto ev = stream->Next(std::chrono::milliseconds(250))) {
            if (!writer->Write(*ev)) {
                stream->Close();
                return grpc::Status::CANCELLED;
            }
            continue;
        }
        if (stream->ended()) {
            stream->Close();
            return grpc::Status::OK;
        }
    }
}

grpc::Status ClientService::GetRun(grpc::ServerContext*, const v1::RunRef* req, v1::RunStatus* reply) {
    if (!parts_.engine) return NoEngine();
    auto st = parts_.engine->Status(req->run_id());
    if (!st) return UnknownRun(req->run_id());
    *reply = std::move(*st);
    return grpc::Status::OK;
}

grpc::Status ClientService::GetReport(grpc::ServerContext*, const v1::RunRef* req, v1::Report* reply) {
    if (!parts_.engine) return NoEngine();
    auto rep = parts_.engine->Report(req->run_id());
    if (!rep) return UnknownRun(req->run_id());
    *reply = std::move(*rep);
    return grpc::Status::OK;
}

grpc::Status ClientService::CancelRun(grpc::ServerContext*, const v1::RunRef* req, v1::Empty*) {
    if (!parts_.engine) return NoEngine();
    if (!parts_.engine->Status(req->run_id())) return UnknownRun(req->run_id());
    parts_.engine->Cancel(req->run_id());
    return grpc::Status::OK;
}

grpc::Status ClientService::ResumeRun(grpc::ServerContext*, const v1::RunRef* req, v1::RunRef* reply) {
    if (!parts_.engine) return NoEngine();
    try {
        reply->set_run_id(parts_.engine->Resume(req->run_id()));
    } catch (const std::invalid_argument& e) {
        return Invalid(e.what());
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::ListRuns(grpc::ServerContext*, const v1::Empty*, v1::RunList* reply) {
    if (!parts_.engine) return NoEngine();
    for (auto& st : parts_.engine->List()) *reply->add_runs() = std::move(st);
    return grpc::Status::OK;
}

grpc::Status ClientService::ForgetCache(grpc::ServerContext*, const v1::ForgetRequest* req, v1::ForgetReply* reply) {
    if (!parts_.cache) return {grpc::StatusCode::UNIMPLEMENTED, "no item cache"};
    v1::CacheForget forget;
    int64_t n = 0;
    if (req->all()) {
        n = static_cast<int64_t>(parts_.cache->ForgetAll());
        forget.set_all(true);
    } else {
        if (!parts_.engine) return NoEngine();
        auto rep = parts_.engine->Report(req->run_id());
        if (!rep) return UnknownRun(req->run_id());
        for (const auto& it : rep->items()) {
            const std::string& key = it.cache().key();
            if (key.empty()) continue;
            n += parts_.cache->Forget(key) ? 1 : 0;
            forget.add_keys(key);
        }
    }
    if (req->team() && parts_.team_cache) parts_.team_cache->Forget(forget);
    reply->set_forgotten(n);
    return grpc::Status::OK;
}

v1::PeerInfo ClientService::SelfInfo() {
    net::Telemetry t;
    {
        std::lock_guard lock(telemetry_mu_);
        t = telemetry_.Sample();
    }
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0) host[0] = '\0';

    v1::PeerInfo self;
    self.set_node_id(options_.node_id);
    self.set_hostname(host);
    self.set_address("local");
    self.set_node_port(options_.node_port);
    self.set_arch(t.arch);
    self.set_cores_total(t.cores_total);
    self.set_cores_free(t.cores_free);
    self.set_mem_available(static_cast<int64_t>(t.mem_available));
    self.set_cpu_load_pct(t.cpu_load_pct);
    self.set_battery_pct(t.battery_pct);
    self.set_on_ac(t.on_ac);
    self.set_mem_total(static_cast<int64_t>(t.mem_total));
    self.set_work_mem(static_cast<int64_t>(t.work_mem));
    self.set_gpus(t.gpus);
    self.set_state("active");
    self.set_is_self(true);
    return self;
}

grpc::Status ClientService::ListPeers(grpc::ServerContext*, const v1::Empty*, v1::PeerList* reply) {
    *reply->add_peers() = SelfInfo();
    if (peers_) {
        // Present = Active + Draining. Draining peers stay visible in status/monitor;
        // placement still uses ListActive() so they do not take new work.
        for (const auto& p : peers_->ListPresent()) {
            if (p.id == options_.node_id) continue;
            v1::PeerInfo info = FromPeer(p);
            if (team_ && team_->pins) {
                info.set_admitted(team_->pins->FingerprintFor(p.id).has_value());
            }
            *reply->add_peers() = std::move(info);
        }
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::ListActivity(grpc::ServerContext*, const v1::Empty*, v1::ActivitySnapshot* reply) {
    if (parts_.engine) {
        size_t finished = 0;
        for (auto& st : parts_.engine->List()) {
            if (st.done() && ++finished > 20) break;
            *reply->add_runs() = std::move(st);
        }
    }
    if (parts_.local_executor) {
        for (const auto& b : parts_.local_executor->ListActive()) {
            auto* a = reply->add_batches();
            a->set_batch(b.spec.name().empty() ? b.batch_id : b.spec.name());
            a->set_requester(b.spec.requester());
            a->set_kind(b.spec.kind());
            a->set_items(b.spec.items_size());
            a->set_done(b.done);
            a->set_current(b.current);
            a->set_cores(std::max(b.spec.resources().cores(), 1));
            a->set_running(b.running);
        }
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::Doctor(grpc::ServerContext*, const v1::Empty*, v1::DoctorReply* reply) {
    if (!parts_.doctor) return {grpc::StatusCode::UNIMPLEMENTED, "no diagnostics"};
    for (auto& f : parts_.doctor()) *reply->add_findings() = std::move(f);
    return grpc::Status::OK;
}

grpc::Status ClientService::TeamCreate(grpc::ServerContext*, const v1::Empty*, v1::TeamSecretReply* reply) {
    if (!team_ || !team_->discovery || !team_->pins) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "team commands are not available"};
    }
    try {
        net::TeamKey key = net::GenerateTeamKey();
        team_->Apply(key);
        reply->set_phrase(net::FormatPhrase(key));
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return {grpc::StatusCode::INTERNAL, e.what()};
    }
}

grpc::Status ClientService::TeamJoin(grpc::ServerContext*, const v1::TeamJoinRequest* req, v1::Empty*) {
    if (!team_ || !team_->discovery || !team_->pins) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "team commands are not available"};
    }
    auto key = net::ParsePhrase(req->phrase());
    if (!key) return {grpc::StatusCode::INVALID_ARGUMENT, "invalid team phrase"};
    try {
        team_->Apply(*key);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return {grpc::StatusCode::INTERNAL, e.what()};
    }
}

grpc::Status ClientService::TeamLeave(grpc::ServerContext*, const v1::Empty*, v1::Empty*) {
    if (!team_ || !team_->discovery || !team_->pins) {
        return {grpc::StatusCode::FAILED_PRECONDITION, "team commands are not available"};
    }
    try {
        team_->Apply(std::nullopt);
        return grpc::Status::OK;
    } catch (const std::exception& e) {
        return {grpc::StatusCode::INTERNAL, e.what()};
    }
}

grpc::Status ClientService::TeamInfo(grpc::ServerContext*, const v1::Empty*, v1::TeamInfoReply* reply) {
    if (!team_) {
        reply->set_in_team(false);
        reply->set_peers_pinned(0);
        return grpc::Status::OK;
    }
    auto key = team_->Get();
    reply->set_in_team(key.has_value());
    if (key) reply->set_team_id(net::TeamId(*key));
    reply->set_peers_pinned(team_->pins ? static_cast<int32_t>(team_->pins->Count()) : 0);
    return grpc::Status::OK;
}

} // namespace zeromesh::node
