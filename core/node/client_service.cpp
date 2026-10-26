#include "node/client_service.hpp"

#include "node/convert.hpp"

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
#include <vector>

namespace zeromesh::node {

namespace {

namespace fs = std::filesystem;

grpc::Status Invalid(const std::string& msg) { return {grpc::StatusCode::INVALID_ARGUMENT, msg}; }

std::string RandomHex(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    randombytes_buf(buf.data(), buf.size());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
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
bool SizeWithinLimit(const fs::path& dir, const std::string& rel, const store::IgnoreRules& rules,
                     uint64_t limit, uint64_t& total) {
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::string child_rel = rel.empty() ? name : rel + "/" + name;
        struct stat st {};
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

// Splits an output path into its segments. nullopt when the path is empty, absolute,
// contains "..", a NUL, or names the work root itself.
std::optional<std::vector<std::string>> SafeSegments(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\0') != std::string::npos) return std::nullopt;
    std::vector<std::string> segs;
    size_t start = 0;
    while (start <= path.size()) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        std::string seg = path.substr(start, slash - start);
        if (seg == "..") return std::nullopt;
        if (!seg.empty() && seg != ".") segs.push_back(std::move(seg));
        start = slash + 1;
    }
    if (segs.empty()) return std::nullopt;
    return segs;
}

std::string Errno(const std::string& what, const fs::path& p) {
    return what + " " + p.string() + ": " + std::strerror(errno);
}

// Creates the directories between `root` and the entry, refusing to pass through a symlink
// or a non-directory. Returns the parent directory of the entry, or an error.
std::optional<fs::path> EnsureParents(const fs::path& root, const std::vector<std::string>& segs,
                                      std::string* error) {
    fs::path cur = root;
    for (size_t i = 0; i + 1 < segs.size(); ++i) {
        cur /= segs[i];
        struct stat st {};
        if (lstat(cur.c_str(), &st) != 0) {
            if (errno != ENOENT || mkdir(cur.c_str(), 0755) != 0) {
                *error = Errno("cannot create directory", cur);
                return std::nullopt;
            }
            continue;
        }
        if (S_ISLNK(st.st_mode)) {
            *error = "refusing to write through symlink " + cur.string();
            return std::nullopt;
        }
        if (!S_ISDIR(st.st_mode)) {
            *error = cur.string() + " exists and is not a directory";
            return std::nullopt;
        }
    }
    return cur;
}

bool CopyBlobTo(const fs::path& src, const fs::path& dst, bool executable, std::string* error) {
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        *error = Errno("cannot open blob", src);
        return false;
    }
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) {
        *error = Errno("cannot create", dst);
        close(in);
        return false;
    }
    std::vector<char> buf(kObjectChunkBytes);
    bool ok = true;
    for (;;) {
        ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            *error = Errno("cannot read blob", src);
            ok = false;
            break;
        }
        if (n == 0) break;
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(out, buf.data() + off, static_cast<size_t>(n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) {
                *error = Errno("cannot write", dst);
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok) break;
    }
    if (ok && fchmod(out, executable ? 0755 : 0644) != 0) {
        *error = Errno("cannot chmod", dst);
        ok = false;
    }
    close(in);
    if (close(out) != 0 && ok) {
        *error = Errno("cannot write", dst);
        ok = false;
    }
    return ok;
}

// Moves `tmp` onto `target`. rename(2) replaces a file or symlink at `target` itself (it never
// follows it); an existing directory there is removed first.
bool ReplaceEntry(const fs::path& tmp, const fs::path& target, std::string* error) {
    struct stat st {};
    if (lstat(target.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        std::error_code ec;
        fs::remove_all(target, ec);
        if (ec) {
            *error = "cannot replace " + target.string() + ": " + ec.message();
            return false;
        }
    }
    if (rename(tmp.c_str(), target.c_str()) != 0) {
        *error = Errno("cannot write", target);
        return false;
    }
    return true;
}

// True when `target` already is a regular file with exactly this content and mode.
bool AlreadyCurrent(const fs::path& target, const v1::OutputEntry& out) {
    struct stat st {};
    if (lstat(target.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (static_cast<int64_t>(st.st_size) != out.digest().size_bytes()) return false;
    if (((st.st_mode & S_IXUSR) != 0) != out.is_executable()) return false;
    return store::CasStore::ComputeFileDigest(target).hash == out.digest().hash();
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
    switch (p.state) {
        case net::PeerState::Active: info.set_state("active"); break;
        case net::PeerState::Draining: info.set_state("draining"); break;
        case net::PeerState::Offline: info.set_state("offline"); break;
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

ClientService::ClientService(std::shared_ptr<store::CasStore> cas, std::shared_ptr<store::MerkleBuilder> merkle,
                             std::shared_ptr<JobManager> jobs, std::shared_ptr<net::PeerTable> peers,
                             ClientServiceOptions options)
    : cas_(std::move(cas)), merkle_(std::move(merkle)), jobs_(std::move(jobs)), peers_(std::move(peers)),
      options_(std::move(options)) {
    if (sodium_init() < 0) throw std::runtime_error("libsodium initialisation failed");
    telemetry_.Sample();  // prime the CPU-load delta
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

grpc::Status ClientService::UploadTree(grpc::ServerContext*, const v1::UploadTreeRequest* req,
                                       v1::UploadTreeReply* reply) {
    const fs::path root = req->path();
    if (!root.is_absolute()) return Invalid("upload path must be absolute: '" + req->path() + "'");
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return Invalid("upload path is not a directory: " + root.string());

    store::IgnoreRules rules = store::IgnoreRules::ForRoot(root);
    if (!req->force_large()) {
        uint64_t total = 0;
        if (!SizeWithinLimit(root, "", rules, options_.upload_limit_bytes, total)) {
            return {grpc::StatusCode::RESOURCE_EXHAUSTED,
                    "refusing to upload " + root.string() + ": more than " + HumanBytes(options_.upload_limit_bytes) +
                        " (" + std::to_string(options_.upload_limit_bytes) +
                        " bytes) after .zmignore; add paths to .zmignore or pass --force-large"};
        }
    }
    try {
        uint64_t total = 0;
        store::Digest d = merkle_->BuildTree(root, rules, &total);
        *reply->mutable_root() = ToProto(d);
        reply->set_total_bytes(static_cast<int64_t>(total));
    } catch (const std::exception& e) {
        return Invalid(std::string("cannot upload ") + root.string() + ": " + e.what());
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::SubmitJob(grpc::ServerContext*, const v1::JobSpec* req, v1::SubmitReply* reply) {
    try {
        reply->set_job_id(jobs_->Submit(*req));
    } catch (const std::invalid_argument& e) {
        return Invalid(e.what());
    } catch (const std::exception& e) {
        return {grpc::StatusCode::INTERNAL, e.what()};
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::WatchJob(grpc::ServerContext* ctx, const v1::JobRef* req,
                                     grpc::ServerWriter<v1::JobEvent>* writer) {
    auto stream = jobs_->Watch(req->job_id());
    if (!stream) return {grpc::StatusCode::NOT_FOUND, "unknown job '" + req->job_id() + "'"};
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
        // nullopt is a timeout or the end. The final event is queued before the job is marked
        // done, so once done every remaining event is already in the stream.
        auto st = jobs_->Get(req->job_id());
        if (!st || st->done()) {
            while (auto ev = stream->Next(std::chrono::milliseconds(0))) {
                if (!writer->Write(*ev)) break;
            }
            stream->Close();
            return grpc::Status::OK;
        }
    }
}

grpc::Status ClientService::GetJob(grpc::ServerContext*, const v1::JobRef* req, v1::JobStatus* reply) {
    auto st = jobs_->Get(req->job_id());
    if (!st) return {grpc::StatusCode::NOT_FOUND, "unknown job '" + req->job_id() + "'"};
    *reply = std::move(*st);
    return grpc::Status::OK;
}

grpc::Status ClientService::CancelJob(grpc::ServerContext*, const v1::JobRef* req, v1::Empty*) {
    if (!jobs_->Get(req->job_id())) return {grpc::StatusCode::NOT_FOUND, "unknown job '" + req->job_id() + "'"};
    jobs_->Cancel(req->job_id());
    return grpc::Status::OK;
}

grpc::Status ClientService::FetchOutputs(grpc::ServerContext*, const v1::FetchRequest* req, v1::FetchReply* reply) {
    const fs::path dest = req->dest_path();
    if (!dest.is_absolute()) return Invalid("dest_path must be absolute: '" + req->dest_path() + "'");
    struct stat dst {};
    if (stat(dest.c_str(), &dst) != 0 || !S_ISDIR(dst.st_mode)) {
        return Invalid("dest_path is not a directory: " + dest.string());
    }
    auto st = jobs_->Get(req->job_id());
    if (!st) return {grpc::StatusCode::NOT_FOUND, "unknown job '" + req->job_id() + "'"};
    auto it = st->results().find(req->task_name());
    if (it == st->results().end()) {
        if (!st->tasks().contains(req->task_name())) {
            return {grpc::StatusCode::NOT_FOUND, "job has no task '" + req->task_name() + "'"};
        }
        return {grpc::StatusCode::FAILED_PRECONDITION, "task '" + req->task_name() + "' has not finished"};
    }
    const v1::TaskResult& result = it->second;

    // Validate every path before touching the disk.
    std::vector<std::vector<std::string>> segments;
    for (const auto& out : result.outputs()) {
        auto segs = SafeSegments(out.path());
        if (!segs) return Invalid("refusing unsafe output path '" + out.path() + "'");
        if (!out.is_symlink() && !out.is_dir() && !store::CasStore::IsValidHash(out.digest().hash())) {
            return {grpc::StatusCode::DATA_LOSS, "output " + out.path() + " has no valid digest"};
        }
        segments.push_back(std::move(*segs));
    }

    for (int i = 0; i < result.outputs_size(); ++i) {
        const v1::OutputEntry& out = result.outputs(i);
        const auto& segs = segments[static_cast<size_t>(i)];
        std::string error;
        auto parent = EnsureParents(dest, segs, &error);
        if (!parent) return {grpc::StatusCode::FAILED_PRECONDITION, error};
        const fs::path target = *parent / segs.back();
        if (req->only_changed() && !out.is_dir() && !out.is_symlink() && AlreadyCurrent(target, out)) continue;

        const fs::path tmp = *parent / (".zm-fetch-" + RandomHex(8));
        bool ok = false;
        if (out.is_symlink()) {
            ok = symlink(out.symlink_target().c_str(), tmp.c_str()) == 0;
            if (!ok) error = Errno("cannot create symlink", tmp);
        } else if (out.is_dir()) {
            ok = merkle_->MaterializeTree(FromProto(out.digest()), tmp);
            if (!ok) error = "cannot materialize directory " + out.path() + " from the CAS";
        } else {
            if (!cas_->HasBlob(out.digest().hash())) {
                return {grpc::StatusCode::DATA_LOSS, "output " + out.path() + " is missing from the CAS"};
            }
            // Make sure an in-memory-only blob is on disk before copying it.
            fs::path src = cas_->GetBlobPath(out.digest().hash());
            if (!fs::exists(src)) {
                auto blob = cas_->GetBlob(out.digest().hash());
                if (blob) cas_->PutBlob(*blob);
            }
            ok = CopyBlobTo(src, tmp, out.is_executable(), &error);
        }
        if (ok) ok = ReplaceEntry(tmp, target, &error);
        if (!ok) {
            std::error_code ec;
            fs::remove_all(tmp, ec);
            return {grpc::StatusCode::INTERNAL, "cannot write output " + out.path() + ": " + error};
        }
        reply->add_written(out.path());
    }
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
    self.set_state("active");
    self.set_is_self(true);
    return self;
}

grpc::Status ClientService::ListPeers(grpc::ServerContext*, const v1::Empty*, v1::PeerList* reply) {
    *reply->add_peers() = SelfInfo();
    if (peers_) {
        for (const auto& p : peers_->ListActive()) {
            if (p.id == options_.node_id) continue;
            *reply->add_peers() = FromPeer(p);
        }
    }
    return grpc::Status::OK;
}

grpc::Status ClientService::TeamCreate(grpc::ServerContext*, const v1::Empty*, v1::TeamSecretReply*) {
    return {grpc::StatusCode::UNIMPLEMENTED, "team commands are not available yet"};
}

grpc::Status ClientService::TeamJoin(grpc::ServerContext*, const v1::TeamJoinRequest*, v1::Empty*) {
    return {grpc::StatusCode::UNIMPLEMENTED, "team commands are not available yet"};
}

grpc::Status ClientService::TeamLeave(grpc::ServerContext*, const v1::Empty*, v1::Empty*) {
    return {grpc::StatusCode::UNIMPLEMENTED, "team commands are not available yet"};
}

grpc::Status ClientService::TeamInfo(grpc::ServerContext*, const v1::Empty*, v1::TeamInfoReply*) {
    return {grpc::StatusCode::UNIMPLEMENTED, "team commands are not available yet"};
}

} // namespace zeromesh::node
