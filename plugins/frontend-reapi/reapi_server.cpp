#include "plugins/frontend-reapi/reapi_server.hpp"
#include "plugins/frontend-reapi/heuristics.hpp"

#include "plugin/client_api_handle.hpp"
#include "zeromesh/v1/client.grpc.pb.h"
#include "zeromesh/v1/common.pb.h"

#include <google/protobuf/any.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace zeromesh::frontend_reapi {

namespace reapi = build::bazel::remote::execution::v2;
namespace zm = zeromesh::v1;

static zm::Digest ToZm(const reapi::Digest& d) {
    zm::Digest z;
    z.set_hash(d.hash());
    z.set_size_bytes(d.size_bytes());
    return z;
}

static reapi::Digest ToReapi(const zm::Digest& d) {
    reapi::Digest r;
    r.set_hash(d.hash());
    r.set_size_bytes(d.size_bytes());
    return r;
}

ReapiServer::ReapiServer(plugin::ClientApiHandle& api) : api_(api) {}
ReapiServer::~ReapiServer() {
    Stop();
}

bool ReapiServer::Start() {
    cas_ = std::make_shared<CasServiceImpl>(api_);
    ac_ = std::make_shared<ActionCacheServiceImpl>();
    exec_ = std::make_shared<ExecutionServiceImpl>(api_, ac_);
    bytestream_ = std::make_shared<ByteStreamServiceImpl>(api_);
    caps_ = std::make_shared<CapabilitiesServiceImpl>();
    int port = 8980;
    if (const char* p = std::getenv("ZM_REAPI_PORT")) {
        try {
            port = std::stoi(p);
        } catch (...) {
        }
    }
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:" + std::to_string(port), grpc::InsecureServerCredentials());
    builder.RegisterService(cas_.get());
    builder.RegisterService(ac_.get());
    builder.RegisterService(exec_.get());
    builder.RegisterService(bytestream_.get());
    builder.RegisterService(caps_.get());
    server_ = builder.BuildAndStart();
    return server_ != nullptr;
}

void ReapiServer::Stop() {
    if (server_) {
        server_->Shutdown();
        server_->Wait();
        server_.reset();
    }
}

// ---- ContentAddressableStorage ----

grpc::Status CasServiceImpl::FindMissingBlobs(grpc::ServerContext*, const reapi::FindMissingBlobsRequest* req,
                                              reapi::FindMissingBlobsResponse* resp) {
    zm::Digests zreq;
    for (const auto& d : req->blob_digests()) {
        *zreq.add_digests() = ToZm(d);
    }
    grpc::ClientContext ctx;
    zm::Digests zresp;
    auto st = api_.stub().FindMissing(&ctx, zreq, &zresp);
    if (!st.ok()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, st.error_message());
    }
    for (const auto& d : zresp.digests()) {
        *resp->add_missing_blob_digests() = ToReapi(d);
    }
    return grpc::Status::OK;
}

grpc::Status CasServiceImpl::BatchUpdateBlobs(grpc::ServerContext*, const reapi::BatchUpdateBlobsRequest* req,
                                              reapi::BatchUpdateBlobsResponse* resp) {
    grpc::ClientContext ctx;
    zm::PutObjectsReply reply;
    auto writer = api_.stub().PutObjects(&ctx, &reply);
    for (const auto& r : req->requests()) {
        zm::Chunk chunk;
        *chunk.mutable_digest() = ToZm(r.digest());
        chunk.set_data(r.data());
        chunk.set_last(true);
        if (!writer->Write(chunk)) break;
    }
    writer->WritesDone();
    auto st = writer->Finish();
    for (const auto& r : req->requests()) {
        auto* out = resp->add_responses();
        *out->mutable_digest() = r.digest();
        if (st.ok()) {
            out->mutable_status()->set_code(0);
        } else {
            out->mutable_status()->set_code(13); // INTERNAL
            out->mutable_status()->set_message(st.error_message());
        }
    }
    return grpc::Status::OK;
}

grpc::Status CasServiceImpl::BatchReadBlobs(grpc::ServerContext*, const reapi::BatchReadBlobsRequest* req,
                                            reapi::BatchReadBlobsResponse* resp) {
    std::cerr << "[frontend-reapi] BatchReadBlobs called with " << req->digests_size() << " digests\n";
    if (req->digests().empty()) return grpc::Status::OK;

    zm::Digests zreq;
    for (const auto& d : req->digests()) {
        *zreq.add_digests() = ToZm(d);
    }

    grpc::ClientContext fctx;
    zm::Digests missing;
    auto fst = api_.stub().FindMissing(&fctx, zreq, &missing);
    if (!fst.ok()) {
        return grpc::Status(grpc::StatusCode::INTERNAL, fst.error_message());
    }

    std::unordered_set<std::string> missing_set;
    for (const auto& d : missing.digests()) {
        missing_set.insert(d.hash());
    }

    zm::Digests to_fetch;
    for (const auto& d : zreq.digests()) {
        if (d.size_bytes() > 0 && missing_set.find(d.hash()) == missing_set.end()) {
            *to_fetch.add_digests() = d;
        }
    }

    std::unordered_map<std::string, std::string> fetched;
    if (to_fetch.digests_size() > 0) {
        grpc::ClientContext gctx;
        auto reader = api_.stub().GetObjects(&gctx, to_fetch);
        std::string cur;
        std::string cur_hash;
        zm::Chunk chunk;
        while (reader->Read(&chunk)) {
            if (chunk.has_digest()) cur_hash = chunk.digest().hash();
            cur += chunk.data();
            if (chunk.last()) {
                fetched[cur_hash] = std::move(cur);
                cur.clear();
            }
        }
        auto gst = reader->Finish();
        if (!gst.ok()) {
            return grpc::Status(grpc::StatusCode::INTERNAL, gst.error_message());
        }
    }

    for (const auto& d : req->digests()) {
        auto* out = resp->add_responses();
        *out->mutable_digest() = d;
        if (missing_set.find(d.hash()) != missing_set.end()) {
            out->mutable_status()->set_code(grpc::StatusCode::NOT_FOUND);
            out->mutable_status()->set_message("not found in CAS");
        } else {
            out->mutable_status()->set_code(0);
            if (d.size_bytes() == 0) {
                out->set_data("");
            } else {
                auto it = fetched.find(d.hash());
                if (it != fetched.end()) out->set_data(it->second);
            }
        }
    }
    return grpc::Status::OK;
}

grpc::Status CasServiceImpl::GetTree(grpc::ServerContext*, const reapi::GetTreeRequest* req,
                                     reapi::GetTreeResponse* resp) {
    if (!req->has_root_digest() || req->root_digest().hash().empty()) {
        return grpc::Status::OK;
    }
    std::vector<reapi::Digest> queue = {req->root_digest()};
    std::unordered_set<std::string> visited;
    while (!queue.empty()) {
        reapi::Digest cur = queue.back();
        queue.pop_back();
        if (!visited.insert(cur.hash()).second) continue;

        zm::Digests zreq;
        *zreq.add_digests() = ToZm(cur);
        grpc::ClientContext ctx;
        auto reader = api_.stub().GetObjects(&ctx, zreq);
        std::string bytes;
        zm::Chunk chunk;
        while (reader->Read(&chunk)) bytes += chunk.data();
        if (!reader->Finish().ok()) continue;

        reapi::Directory dir;
        if (dir.ParseFromString(bytes)) {
            *resp->add_directories() = dir;
            for (const auto& child : dir.directories()) {
                queue.push_back(child.digest());
            }
        }
    }
    return grpc::Status::OK;
}

// ---- ActionCache ----

grpc::Status ActionCacheServiceImpl::GetActionResult(grpc::ServerContext*, const reapi::GetActionResultRequest* req,
                                                     reapi::ActionResult* resp) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(req->action_digest().hash());
    if (it == cache_.end()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "action result not cached");
    }
    *resp = it->second;
    return grpc::Status::OK;
}

grpc::Status ActionCacheServiceImpl::UpdateActionResult(grpc::ServerContext*,
                                                        const reapi::UpdateActionResultRequest* req,
                                                        reapi::ActionResult* resp) {
    std::lock_guard<std::mutex> lk(mu_);
    cache_[req->action_digest().hash()] = req->action_result();
    *resp = req->action_result();
    return grpc::Status::OK;
}

// ---- Execution ----

std::string ExecutionServiceImpl::FetchBlob(const reapi::Digest& d) {
    if (d.size_bytes() == 0) return {};
    zm::Digests zreq;
    *zreq.add_digests() = ToZm(d);
    grpc::ClientContext ctx;
    auto reader = api_.stub().GetObjects(&ctx, zreq);
    std::string out, cur;
    zm::Chunk chunk;
    while (reader->Read(&chunk)) {
        cur += chunk.data();
        if (chunk.last()) {
            out += cur;
            cur.clear();
        }
    }
    reader->Finish();
    return out;
}

namespace {

// REAPI Directory entry names must be a single path segment (no traversal).
bool SafeEntryName(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
    if (name.find('\0') != std::string::npos) return false;
    return true;
}

} // namespace

void ExecutionServiceImpl::MaterializeDir(const reapi::Digest& dirDigest, const std::filesystem::path& base) {
    std::string bytes = FetchBlob(dirDigest);
    reapi::Directory dir;
    if (!dir.ParseFromString(bytes)) {
        std::cerr << "[frontend-reapi] failed to parse Directory for " << dirDigest.hash() << " (bytes=" << bytes.size()
                  << ")\n";
        return;
    }
    std::filesystem::create_directories(base);
    std::cerr << "[frontend-reapi] MaterializeDir at " << base.string() << " files=" << dir.files_size()
              << " dirs=" << dir.directories_size() << " symlinks=" << dir.symlinks_size() << "\n";
    for (const auto& f : dir.files()) {
        if (!SafeEntryName(f.name())) {
            std::cerr << "[frontend-reapi] refusing unsafe file name '" << f.name() << "'\n";
            continue;
        }
        std::cerr << "[frontend-reapi]   file: " << f.name() << " digest=" << f.digest().hash() << "\n";
        std::string content = FetchBlob(f.digest());
        std::filesystem::path p = base / f.name();
        std::ofstream ofs(p, std::ios::binary);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        ofs.close();
        if (f.is_executable()) {
            std::filesystem::permissions(p, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
        }
    }
    for (const auto& sd : dir.directories()) {
        if (!SafeEntryName(sd.name())) {
            std::cerr << "[frontend-reapi] refusing unsafe directory name '" << sd.name() << "'\n";
            continue;
        }
        std::cerr << "[frontend-reapi]   subdir: " << sd.name() << " digest=" << sd.digest().hash() << "\n";
        MaterializeDir(sd.digest(), base / sd.name());
    }
    for (const auto& sl : dir.symlinks()) {
        if (!SafeEntryName(sl.name())) {
            std::cerr << "[frontend-reapi] refusing unsafe symlink name '" << sl.name() << "'\n";
            continue;
        }
        // Absolute or parent-escaping targets can point outside the tree; refuse them.
        const std::string& target = sl.target();
        if (target.empty() || target.front() == '/' || target.find("..") != std::string::npos) {
            std::cerr << "[frontend-reapi] refusing unsafe symlink target '" << target << "'\n";
            continue;
        }
        std::cerr << "[frontend-reapi]   symlink: " << sl.name() << " -> " << target << "\n";
        std::filesystem::create_symlink(target, base / sl.name());
    }
}

grpc::Status ExecutionServiceImpl::Execute(grpc::ServerContext*, const reapi::ExecuteRequest* req,
                                           grpc::ServerWriter<reapi::Operation>* writer) {
    reapi::Operation op;
    op.set_name("operations/" + req->action_digest().hash());

    // Check action cache first (unless skip_cache_lookup).
    if (!req->skip_cache_lookup()) {
        reapi::GetActionResultRequest gar;
        *gar.mutable_action_digest() = req->action_digest();
        reapi::ActionResult cached;
        grpc::ServerContext cctx;
        if (ac_->GetActionResult(&cctx, &gar, &cached).ok()) {
            reapi::ExecuteResponse resp;
            *resp.mutable_result() = cached;
            resp.set_cached_result(true);
            op.set_done(true);
            (void)op.mutable_response()->PackFrom(resp);
            writer->Write(op);
            return grpc::Status::OK;
        }
    }

    // Fetch Action.
    std::string actionBytes = FetchBlob(req->action_digest());
    reapi::Action action;
    if (!action.ParseFromString(actionBytes)) {
        op.set_done(true);
        writer->Write(op);
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "cannot parse Action");
    }

    // Fetch Command.
    std::string cmdBytes = FetchBlob(action.command_digest());
    reapi::Command command;
    if (!command.ParseFromString(cmdBytes)) {
        op.set_done(true);
        writer->Write(op);
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "cannot parse Command");
    }

    // Materialize input root into a temp dir and upload as ZeroMesh tree.
    zm::Digest inputRoot;
    bool hasInput = action.has_input_root_digest() && !action.input_root_digest().hash().empty();
    if (hasInput) {
        char tmpl[] = "/tmp/zmreapi-XXXXXX";
        char* d = mkdtemp(tmpl);
        if (d) {
            std::filesystem::path base(d);
            MaterializeDir(action.input_root_digest(), base);
            // UploadTree via ClientApi.
            grpc::ClientContext uctx;
            zm::UploadTreeRequest ureq;
            ureq.set_path(base.string());
            ureq.set_force_large(true);
            zm::UploadTreeReply ureply;
            auto ust = api_.stub().UploadTree(&uctx, ureq, &ureply);
            if (ust.ok()) inputRoot = ureply.root();
            std::filesystem::remove_all(base);
        }
    }

    // Build TaskSpec.
    zm::TaskSpec spec;
    spec.set_name(req->action_digest().hash());
    std::vector<std::string> argv;
    for (const auto& a : command.arguments()) {
        spec.add_argv(a);
        argv.push_back(a);
    }
    for (const auto& e : command.environment_variables()) {
        (*spec.mutable_env())[e.name()] = e.value();
    }
    if (hasInput && !inputRoot.hash().empty()) {
        *spec.mutable_input_root() = inputRoot;
    }
    std::cerr << "[frontend-reapi] command.working_directory: '" << command.working_directory() << "'\n";
    spec.set_working_dir(command.working_directory().empty() ? "." : command.working_directory());
    for (const auto& o : command.output_files()) spec.add_outputs(o);
    for (const auto& o : command.output_directories()) spec.add_outputs(o);
    for (const auto& o : command.output_paths()) spec.add_outputs(o);

    zm::EnvSpec* es = spec.mutable_env_spec();
    es->set_name("host");
    zm::Resources* rs = spec.mutable_resources();
    rs->set_cores(1);
    rs->set_mem_bytes(0);
    rs->set_arch("");

    bool critical = IsCriticalPath(argv);
    zm::Policy* pol = spec.mutable_policy();
    pol->set_may_run_local(critical);
    pol->set_may_run_remote(!critical);
    pol->set_max_retries(1);
    pol->set_timeout_s(3600);
    pol->set_allow_network(false);
    pol->set_preemptible(true);

    zm::JobSpec job;
    *job.add_tasks() = spec;

    grpc::ClientContext sctx;
    zm::SubmitReply sreply;
    auto sst = api_.stub().SubmitJob(&sctx, job, &sreply);
    if (!sst.ok()) {
        op.set_done(true);
        writer->Write(op);
        return grpc::Status(grpc::StatusCode::INTERNAL, "SubmitJob failed: " + sst.error_message());
    }
    std::string jobId = sreply.job_id();

    // Watch job until completion.
    zm::JobRef jref;
    jref.set_job_id(jobId);
    grpc::ClientContext wctx;
    auto reader = api_.stub().WatchJob(&wctx, jref);
    zm::JobEvent ev;
    zm::TaskResult result;
    while (reader->Read(&ev)) {
        if (ev.has_result()) result = ev.result();
        if (ev.state() == zm::SUCCEEDED || ev.state() == zm::FAILED || ev.state() == zm::CANCELLED) {
            if (ev.has_result()) result = ev.result();
            break;
        }
    }
    reader->Finish();

    std::cerr << "[frontend-reapi] task " << (argv.empty() ? "" : argv[0]) << " critical=" << critical
              << " executed_by=" << result.executed_by() << " exit=" << result.exit_code()
              << " error=" << result.error() << " stderr=" << result.stderr_head() << "\n";

    // Build ActionResult.
    reapi::ActionResult ar;
    ar.set_exit_code(result.exit_code());
    if (!result.stdout().hash().empty()) *ar.mutable_stdout_digest() = ToReapi(result.stdout());
    if (!result.stderr().hash().empty()) *ar.mutable_stderr_digest() = ToReapi(result.stderr());
    for (const auto& o : result.outputs()) {
        std::cerr << "[frontend-reapi] OUT: " << o.path() << " digest=" << o.digest().hash()
                  << " size=" << o.digest().size_bytes() << "\n";
        if (o.is_dir()) {
            auto* od = ar.add_output_directories();
            od->set_path(o.path());
            *od->mutable_tree_digest() = ToReapi(o.digest());
            *od->mutable_root_directory_digest() = ToReapi(o.digest());
        } else {
            auto* of = ar.add_output_files();
            of->set_path(o.path());
            *of->mutable_digest() = ToReapi(o.digest());
            of->set_is_executable(o.is_executable());
        }
    }

    // Cache successful results only (REAPI ActionCache semantics).
    if (ar.exit_code() == 0) {
        reapi::UpdateActionResultRequest uar;
        *uar.mutable_action_digest() = req->action_digest();
        *uar.mutable_action_result() = ar;
        reapi::ActionResult ignored;
        grpc::ServerContext cctx;
        ac_->UpdateActionResult(&cctx, &uar, &ignored);
    }

    op.set_done(true);
    reapi::ExecuteResponse resp;
    *resp.mutable_result() = ar;
    resp.set_cached_result(false);
    (void)op.mutable_response()->PackFrom(resp);
    writer->Write(op);
    return grpc::Status::OK;
}

grpc::Status ExecutionServiceImpl::WaitExecution(grpc::ServerContext*, const reapi::WaitExecutionRequest* req,
                                                 grpc::ServerWriter<reapi::Operation>* writer) {
    reapi::Operation op;
    op.set_name(req->name());
    op.set_done(true);
    writer->Write(op);
    return grpc::Status::OK;
}

// ---- ByteStream ----

static bool ParseResourceName(std::string_view res, std::string* hash, int64_t* size) {
    auto pos = res.find("blobs/");
    if (pos == std::string_view::npos) return false;
    res = res.substr(pos + 6);
    auto slash = res.find('/');
    if (slash == std::string_view::npos) {
        if (hash) *hash = std::string(res);
        if (size) *size = 0;
        return true;
    }
    if (hash) *hash = std::string(res.substr(0, slash));
    if (size) {
        try {
            *size = std::stoll(std::string(res.substr(slash + 1)));
        } catch (...) {
            *size = 0;
        }
    }
    return true;
}

grpc::Status ByteStreamServiceImpl::Read(grpc::ServerContext*, const google::bytestream::ReadRequest* req,
                                         grpc::ServerWriter<google::bytestream::ReadResponse>* writer) {
    std::cerr << "[frontend-reapi] ByteStream::Read req: " << req->resource_name() << "\n";
    std::string hash;
    int64_t size = 0;
    if (!ParseResourceName(req->resource_name(), &hash, &size)) {
        std::cerr << "[frontend-reapi] ByteStream::Read failed to parse: " << req->resource_name() << "\n";
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid resource name");
    }

    if (size == 0) {
        return grpc::Status::OK;
    }

    zm::Digests zreq;
    auto* d = zreq.add_digests();
    d->set_hash(hash);
    d->set_size_bytes(size);

    grpc::ClientContext ctx;
    auto reader = api_.stub().GetObjects(&ctx, zreq);
    zm::Chunk chunk;
    while (reader->Read(&chunk)) {
        google::bytestream::ReadResponse resp;
        resp.set_data(chunk.data());
        if (!writer->Write(resp)) break;
    }
    auto st = reader->Finish();
    if (!st.ok()) {
        std::cerr << "[frontend-reapi] ByteStream::Read failed: " << st.error_message() << "\n";
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "blob not found");
    }
    std::cerr << "[frontend-reapi] ByteStream::Read success for " << hash << "\n";
    return grpc::Status::OK;
}

grpc::Status ByteStreamServiceImpl::Write(grpc::ServerContext*,
                                          grpc::ServerReader<google::bytestream::WriteRequest>* reader,
                                          google::bytestream::WriteResponse* resp) {
    google::bytestream::WriteRequest req;
    std::string hash;
    int64_t size = 0;
    bool first = true;
    grpc::ClientContext cctx;
    zm::PutObjectsReply reply;
    std::unique_ptr<grpc::ClientWriter<zm::Chunk>> writer;

    int64_t total = 0;
    while (reader->Read(&req)) {
        if (first) {
            first = false;
            if (!ParseResourceName(req.resource_name(), &hash, &size)) {
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "invalid resource name");
            }
            writer = api_.stub().PutObjects(&cctx, &reply);
        }
        zm::Chunk chunk;
        if (chunk.data().empty() && total == 0) {
            chunk.mutable_digest()->set_hash(hash);
            chunk.mutable_digest()->set_size_bytes(size);
        }
        chunk.set_data(req.data());
        total += static_cast<int64_t>(req.data().size());
        chunk.set_last(req.finish_write());
        if (writer && !writer->Write(chunk)) break;
    }
    if (writer) {
        writer->WritesDone();
        writer->Finish();
    }
    resp->set_committed_size(total);
    return grpc::Status::OK;
}

grpc::Status ByteStreamServiceImpl::QueryWriteStatus(grpc::ServerContext*,
                                                     const google::bytestream::QueryWriteStatusRequest* req,
                                                     google::bytestream::QueryWriteStatusResponse* resp) {
    std::string hash;
    int64_t size = 0;
    ParseResourceName(req->resource_name(), &hash, &size);
    resp->set_committed_size(size);
    resp->set_complete(true);
    return grpc::Status::OK;
}

// ---- Capabilities ----

grpc::Status CapabilitiesServiceImpl::GetCapabilities(grpc::ServerContext*, const reapi::GetCapabilitiesRequest*,
                                                      reapi::ServerCapabilities* resp) {
    auto* cc = resp->mutable_cache_capabilities();
    cc->add_digest_functions(reapi::DigestFunction::SHA256);
    cc->mutable_action_cache_update_capabilities()->set_update_enabled(true);
    cc->set_max_batch_total_size_bytes(4 * 1024 * 1024);

    auto* ec = resp->mutable_execution_capabilities();
    ec->set_digest_function(reapi::DigestFunction::SHA256);
    ec->set_exec_enabled(true);

    auto* low = resp->mutable_low_api_version();
    low->set_major(2);
    low->set_minor(0);
    low->set_patch(0);

    auto* high = resp->mutable_high_api_version();
    high->set_major(2);
    high->set_minor(2);
    high->set_patch(0);

    return grpc::Status::OK;
}

} // namespace zeromesh::frontend_reapi
