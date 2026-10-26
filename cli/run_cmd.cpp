#include "cli/run_cmd.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

namespace zeromesh::cli {

const char* const kRunUsage =
    "Usage: zm run [-o PATH]... [--remote-only|--local-only] [--cores N] [--mem SIZE]\n"
    "              [--env NAME] [--force-large] -- COMMAND [ARGS...]\n"
    "\n"
    "Uploads the current directory, runs COMMAND in a sandbox (anywhere in the team), streams\n"
    "its output and writes its outputs back. Without -o, every file the command creates or\n"
    "modifies is an output. Exits with the command's exit code.\n";

namespace {

void WriteAll(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = write(fd, data.data(), data.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
        data.remove_prefix(static_cast<size_t>(n));
    }
}

int g_sigpipe[2] = {-1, -1};
std::atomic<int> g_interrupts{0};

void OnInterrupt(int) {
    if (g_interrupts.fetch_add(1) >= 1) _exit(130);  // second Ctrl-C: give up at once
    char c = 1;
    ssize_t n = write(g_sigpipe[1], &c, 1);
    (void)n;
}

// While alive, the first SIGINT/SIGTERM cancels the job instead of killing zm, so the
// daemon stops the task and zm still reports how it ended.
class CancelOnInterrupt {
public:
    CancelOnInterrupt(v1::ClientApi::Stub& stub, std::string job_id) {
        if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) != 0) return;
        done_ = eventfd(0, EFD_CLOEXEC);
        struct sigaction sa {};
        sa.sa_handler = OnInterrupt;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, &old_int_);
        sigaction(SIGTERM, &sa, &old_term_);
        thread_ = std::thread([&stub, job_id = std::move(job_id), done = done_] {
            pollfd fds[2] = {{g_sigpipe[0], POLLIN, 0}, {done, POLLIN, 0}};
            for (;;) {
                int rc = poll(fds, 2, -1);
                if (rc < 0 && errno == EINTR) continue;
                if (rc < 0 || (fds[1].revents & POLLIN)) return;
                if (fds[0].revents & POLLIN) {
                    std::cerr << "\nzm: cancelling (Ctrl-C again to quit at once)\n";
                    grpc::ClientContext ctx;
                    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                    v1::JobRef ref;
                    ref.set_job_id(job_id);
                    v1::Empty empty;
                    stub.CancelJob(&ctx, ref, &empty);
                    return;
                }
            }
        });
    }

    ~CancelOnInterrupt() {
        if (!thread_.joinable()) return;
        uint64_t one = 1;
        ssize_t n = write(done_, &one, sizeof(one));
        (void)n;
        thread_.join();
        sigaction(SIGINT, &old_int_, nullptr);
        sigaction(SIGTERM, &old_term_, nullptr);
        close(done_);
        close(g_sigpipe[0]);
        close(g_sigpipe[1]);
    }

    CancelOnInterrupt(const CancelOnInterrupt&) = delete;
    CancelOnInterrupt& operator=(const CancelOnInterrupt&) = delete;

private:
    int done_{-1};
    struct sigaction old_int_ {};
    struct sigaction old_term_ {};
    std::thread thread_;
};

// A cache hit streams no logs, so replay the stored stdout/stderr from the CAS.
void ReplayLogs(Client& client, const v1::TaskResult& r) {
    v1::Digests req;
    if (!r.stdout().hash().empty()) *req.add_digests() = r.stdout();
    if (!r.stderr().hash().empty()) *req.add_digests() = r.stderr();
    if (req.digests_size() == 0) return;
    grpc::ClientContext ctx;
    auto reader = client.stub().GetObjects(&ctx, req);
    v1::Chunk c;
    int fd = 1;
    bool any = false;
    while (reader->Read(&c)) {
        if (c.has_digest()) fd = c.digest().hash() == r.stdout().hash() ? 1 : 2;
        WriteAll(fd, c.data());
        any = true;
    }
    if (!reader->Finish().ok() && !any) {
        WriteAll(1, r.stdout_head());
        WriteAll(2, r.stderr_head());
    }
}

} // namespace

std::optional<int64_t> ParseSize(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    if (i == 0) return std::nullopt;
    char* end = nullptr;
    std::string num = s.substr(0, i);
    double v = std::strtod(num.c_str(), &end);
    if (end != num.c_str() + num.size() || !std::isfinite(v) || v < 0) return std::nullopt;
    std::string unit;
    for (char c : s.substr(i)) unit.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    double mult = 1;
    if (unit.empty() || unit == "B") mult = 1;
    else if (unit == "K" || unit == "KB" || unit == "KIB") mult = 1024.0;
    else if (unit == "M" || unit == "MB" || unit == "MIB") mult = 1024.0 * 1024;
    else if (unit == "G" || unit == "GB" || unit == "GIB") mult = 1024.0 * 1024 * 1024;
    else if (unit == "T" || unit == "TB" || unit == "TIB") mult = 1024.0 * 1024 * 1024 * 1024;
    else return std::nullopt;
    double bytes = v * mult;
    if (bytes > 9.0e18) return std::nullopt;
    return static_cast<int64_t>(bytes);
}

std::optional<RunOptions> ParseRunArgs(const std::vector<std::string>& args, std::string* error) {
    RunOptions o;
    size_t i = 0;
    auto value = [&](const std::string& flag) -> std::optional<std::string> {
        if (i + 1 >= args.size()) {
            *error = flag + " needs a value";
            return std::nullopt;
        }
        return args[++i];
    };
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--") {
            ++i;
            break;
        }
        if (a == "-o" || a == "--output") {
            auto v = value(a);
            if (!v) return std::nullopt;
            o.outputs.push_back(*v);
        } else if (a == "--remote-only") {
            o.remote_only = true;
        } else if (a == "--local-only") {
            o.local_only = true;
        } else if (a == "--cores") {
            auto v = value(a);
            if (!v) return std::nullopt;
            char* end = nullptr;
            long n = std::strtol(v->c_str(), &end, 10);
            if (end == v->c_str() || *end != '\0' || n < 1 || n > 4096) {
                *error = "--cores needs a positive integer, got '" + *v + "'";
                return std::nullopt;
            }
            o.cores = static_cast<int>(n);
        } else if (a == "--mem") {
            auto v = value(a);
            if (!v) return std::nullopt;
            auto n = ParseSize(*v);
            if (!n) {
                *error = "--mem needs a size such as 512M or 2G, got '" + *v + "'";
                return std::nullopt;
            }
            o.mem_bytes = *n;
        } else if (a == "--env") {
            auto v = value(a);
            if (!v) return std::nullopt;
            o.env = *v;
        } else if (a == "--force-large") {
            o.force_large = true;
        } else if (!a.empty() && a[0] == '-') {
            *error = "unknown option '" + a + "'";
            return std::nullopt;
        } else {
            break;  // the command starts here (no "--")
        }
    }
    o.argv.assign(args.begin() + static_cast<std::ptrdiff_t>(std::min(i, args.size())), args.end());
    if (o.remote_only && o.local_only) {
        *error = "--remote-only and --local-only exclude each other";
        return std::nullopt;
    }
    if (o.argv.empty()) {
        *error = "missing command (zm run [options] -- COMMAND ...)";
        return std::nullopt;
    }
    return o;
}

int RunCommand(Client& client, const RunOptions& opts) {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) {
        std::cerr << "zm: cannot determine the current directory: " << ec.message() << "\n";
        return 1;
    }

    v1::UploadTreeReply up;
    {
        grpc::ClientContext ctx;
        v1::UploadTreeRequest req;
        req.set_path(cwd.string());
        req.set_force_large(opts.force_large);
        grpc::Status s = client.stub().UploadTree(&ctx, req, &up);
        if (!s.ok()) return ReportRpcError("upload failed", s);
    }

    v1::JobSpec job;
    job.set_name("zm run");
    v1::TaskSpec* t = job.add_tasks();
    t->set_name("run");
    for (const auto& a : opts.argv) t->add_argv(a);
    *t->mutable_input_root() = up.root();
    t->set_working_dir(".");
    for (const auto& o : opts.outputs) t->add_outputs(o);
    t->set_capture_changed(opts.outputs.empty());
    t->mutable_env_spec()->set_name(opts.env);
    if (opts.cores > 0) t->mutable_resources()->set_cores(opts.cores);
    if (opts.mem_bytes > 0) t->mutable_resources()->set_mem_bytes(opts.mem_bytes);
    t->mutable_policy()->set_may_run_local(!opts.remote_only);
    t->mutable_policy()->set_may_run_remote(!opts.local_only);

    std::string job_id;
    {
        grpc::ClientContext ctx;
        v1::SubmitReply reply;
        grpc::Status s = client.stub().SubmitJob(&ctx, job, &reply);
        if (!s.ok()) return ReportRpcError("submit failed", s);
        job_id = reply.job_id();
    }

    std::optional<v1::TaskResult> result;
    {
        CancelOnInterrupt guard(client.stub(), job_id);
        grpc::ClientContext ctx;
        v1::JobRef ref;
        ref.set_job_id(job_id);
        auto reader = client.stub().WatchJob(&ctx, ref);
        v1::JobEvent ev;
        while (reader->Read(&ev)) {
            if (ev.log_fd() == 1 || ev.log_fd() == 2) {
                WriteAll(ev.log_fd(), ev.log_chunk());
            } else if (ev.task_name() == "run" && ev.has_result() &&
                       (ev.state() == v1::SUCCEEDED || ev.state() == v1::FAILED || ev.state() == v1::CANCELLED)) {
                result = ev.result();
            }
        }
        grpc::Status s = reader->Finish();
        if (!s.ok() && !result) return ReportRpcError("watching the job failed", s);
    }
    if (!result) {
        std::cerr << "zm: the job ended without a result\n";
        return 1;
    }

    if (result->cache_hit()) ReplayLogs(client, *result);
    if (result->cancelled()) {
        std::cerr << "zm: cancelled\n";
        return 130;
    }
    if (!result->error().empty()) {
        std::cerr << "zm: " << result->error() << "\n";
        return 1;
    }
    if (result->timed_out()) {
        std::cerr << "zm: the command timed out\n";
        return 1;
    }

    {
        grpc::ClientContext ctx;
        v1::FetchRequest req;
        req.set_job_id(job_id);
        req.set_task_name("run");
        req.set_dest_path(cwd.string());
        req.set_only_changed(true);
        v1::FetchReply reply;
        grpc::Status s = client.stub().FetchOutputs(&ctx, req, &reply);
        if (!s.ok()) return ReportRpcError("cannot write outputs back", s);
    }
    return result->exit_code();
}

} // namespace zeromesh::cli
