#include "cli/run_cmd.hpp"

#include "exec/subprocess.hpp"

#include <google/protobuf/util/json_util.h>

#include <fcntl.h>
#include <glob.h>
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
#include <iostream>
#include <map>
#include <string_view>
#include <fstream>
#include <sstream>
#include <thread>

namespace zeromesh::cli {

const char* const kRunUsage =
    "Usage: zm run [-o PATH]... [--remote-only|--local-only] [--cores N] [--mem SIZE] [--gpu N]\n"
    "              [--env host|capture|sha256:TREE] [--capture PATH]... [--needs-version CONSTRAINT]\n"
    "              [--keep-going] [--no-cache] [--report FILE] [--force-large] [--each GLOB] -- COMMAND [ARGS...]\n"
    "       zm run --from run.json\n"
    "\n"
    "Snapshots the current directory, runs COMMAND in a sandbox (anywhere in the team), streams its\n"
    "output and writes its outputs back. Without -o, every file the command creates or modifies is\n"
    "an output. Exits with the command's exit code.\n"
    "\n"
    "With --each GLOB, one item runs per match, with every {} in COMMAND replaced by the match path\n"
    "relative to the current directory. All items share one snapshot. Exit is non-zero if any item\n"
    "failed; failures are listed on stderr.\n"
    "\n"
    "--env capture captures COMMAND's program (and any --capture PATH) on this machine and ships it,\n"
    "so donors without the software can run it. --needs-version accepts any donor whose program\n"
    "version satisfies CONSTRAINT (e.g. \">=4.0\") instead of the exact build installed here.\n"
    "\n"
    "--from runs a whole run described in JSON (a RunSpec: workspace_path, stages, items, policy).\n";

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
    if (g_interrupts.fetch_add(1) >= 1) _exit(130); // second Ctrl-C: give up at once
    char c = 1;
    ssize_t n = write(g_sigpipe[1], &c, 1);
    (void)n;
}

// While alive, the first SIGINT/SIGTERM cancels the run instead of killing zm, so the daemon
// stops it (finished items stay) and zm still reports how it ended.
class CancelOnInterrupt {
public:
    CancelOnInterrupt(v1::ClientApi::Stub& stub, std::string run_id) {
        if (pipe2(g_sigpipe, O_CLOEXEC | O_NONBLOCK) != 0) return;
        done_ = eventfd(0, EFD_CLOEXEC);
        struct sigaction sa{};
        sa.sa_handler = OnInterrupt;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, &old_int_);
        sigaction(SIGTERM, &sa, &old_term_);
        thread_ = std::thread([&stub, run_id = std::move(run_id), done = done_] {
            pollfd fds[2] = {{g_sigpipe[0], POLLIN, 0}, {done, POLLIN, 0}};
            for (;;) {
                int rc = poll(fds, 2, -1);
                if (rc < 0 && errno == EINTR) continue;
                if (rc < 0 || (fds[1].revents & POLLIN)) return;
                if (fds[0].revents & POLLIN) {
                    std::cerr << "\nzm: cancelling the run (Ctrl-C again to quit at once)\n";
                    grpc::ClientContext ctx;
                    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                    v1::RunRef ref;
                    ref.set_run_id(run_id);
                    v1::Empty empty;
                    stub.CancelRun(&ctx, ref, &empty);
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
    struct sigaction old_int_{};
    struct sigaction old_term_{};
    std::thread thread_;
};

// A cache hit streams no logs, so replay the stored stdout/stderr from the store.
void ReplayLogs(Client& client, const v1::ItemResult& r) {
    v1::Digests req;
    if (!r.stdout().hash().empty() && r.stdout().size_bytes() > 0) *req.add_digests() = r.stdout();
    if (!r.stderr().hash().empty() && r.stderr().size_bytes() > 0) *req.add_digests() = r.stderr();
    if (req.digests_size() == 0) return;
    grpc::ClientContext ctx;
    auto reader = client.stub().GetObjects(&ctx, req);
    v1::Chunk c;
    int fd = 1;
    while (reader->Read(&c)) {
        if (c.has_digest()) fd = c.digest().hash() == r.stdout().hash() ? 1 : 2;
        WriteAll(fd, c.data());
    }
    reader->Finish();
}

const char* KindWord(v1::FailureKind k) {
    switch (k) {
    case v1::FAILURE_EXIT:
        return "exit";
    case v1::FAILURE_MISSING_OUTPUT:
        return "missing output";
    case v1::FAILURE_OOM:
        return "out of memory";
    case v1::FAILURE_TIMEOUT:
        return "timeout";
    case v1::FAILURE_HUNG:
        return "hung";
    case v1::FAILURE_ENV_UNAVAILABLE:
        return "software unavailable";
    case v1::FAILURE_TOOLCHAIN_MISMATCH:
        return "toolchain mismatch";
    case v1::FAILURE_SANDBOX:
        return "sandbox";
    case v1::FAILURE_DONOR_LEFT:
        return "donor left";
    case v1::FAILURE_CANCELLED:
        return "cancelled";
    case v1::FAILURE_DEPENDENCY:
        return "dependency";
    case v1::FAILURE_NO_CAPACITY:
        return "no node can take it";
    default:
        return "failed";
    }
}

std::string Describe(const std::string& label, const v1::ItemResult& r) {
    const auto& f = r.failure();
    std::string s = label + ": " + KindWord(f.kind());
    if (!f.message().empty() && f.message() != KindWord(f.kind())) s += ": " + f.message();
    return s;
}

int ExitCodeOf(const v1::ItemResult& r) {
    if (r.state() == v1::ITEM_SUCCEEDED) return 0;
    if (r.state() == v1::ITEM_CANCELLED) return 130;
    if (r.failure().kind() == v1::FAILURE_EXIT && r.failure().exit_code() > 0) return r.failure().exit_code();
    return 1;
}

std::optional<std::string> ReadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

std::string ReplaceBraces(std::string_view s, std::string_view replacement) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '{' && s[i + 1] == '}') {
            out.append(replacement);
            i += 2;
        } else {
            out.push_back(s[i]);
            ++i;
        }
    }
    return out;
}

bool ExpandEachGlob(const std::string& pattern, const std::filesystem::path& cwd, std::vector<std::string>* matches,
                    std::string* error) {
    if (pattern.empty()) {
        *error = "--each needs a non-empty glob";
        return false;
    }
    const std::filesystem::path abs =
        (std::filesystem::path(pattern).is_absolute()) ? std::filesystem::path(pattern) : cwd / pattern;

    glob_t g{};
    const int rc = glob(abs.c_str(), GLOB_NOSORT | GLOB_ERR, nullptr, &g);
    if (rc == GLOB_NOMATCH) {
        globfree(&g);
        *error = "--each matched no files: " + pattern;
        return false;
    }
    if (rc != 0) {
        globfree(&g);
        *error = "--each glob failed: " + pattern;
        return false;
    }

    matches->clear();
    matches->reserve(g.gl_pathc);
    std::error_code ec;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        std::filesystem::path p = g.gl_pathv[i];
        std::filesystem::path rel = std::filesystem::relative(p, cwd, ec);
        if (ec) {
            globfree(&g);
            *error = "--each: cannot relativize " + p.string() + ": " + ec.message();
            return false;
        }
        matches->push_back(rel.generic_string());
    }
    globfree(&g);
    std::sort(matches->begin(), matches->end());
    matches->erase(std::unique(matches->begin(), matches->end()), matches->end());
    if (matches->empty()) {
        *error = "--each matched no files: " + pattern;
        return false;
    }
    return true;
}

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
    if (unit.empty() || unit == "B")
        mult = 1;
    else if (unit == "K" || unit == "KB" || unit == "KIB")
        mult = 1024.0;
    else if (unit == "M" || unit == "MB" || unit == "MIB")
        mult = 1024.0 * 1024;
    else if (unit == "G" || unit == "GB" || unit == "GIB")
        mult = 1024.0 * 1024 * 1024;
    else if (unit == "T" || unit == "TB" || unit == "TIB")
        mult = 1024.0 * 1024 * 1024 * 1024;
    else
        return std::nullopt;
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
    auto positive = [&](const std::string& flag, const std::string& v, int max) -> std::optional<int> {
        char* end = nullptr;
        long n = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0' || n < 0 || n > max) {
            *error = flag + " needs a number, got '" + v + "'";
            return std::nullopt;
        }
        return static_cast<int>(n);
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
            auto n = positive(a, *v, 4096);
            if (!n || *n < 1) {
                if (error->empty()) *error = "--cores needs a positive integer, got '" + *v + "'";
                return std::nullopt;
            }
            o.cores = *n;
        } else if (a == "--gpu") {
            auto v = value(a);
            if (!v) return std::nullopt;
            auto n = positive(a, *v, 64);
            if (!n) return std::nullopt;
            o.gpus = *n;
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
            if (*v != "host" && *v != "capture" && v->rfind("sha256:", 0) != 0) {
                *error = "--env is host, capture or sha256:TREE, got '" + *v + "'";
                return std::nullopt;
            }
            o.env = *v;
        } else if (a == "--capture") {
            auto v = value(a);
            if (!v) return std::nullopt;
            std::error_code ec;
            o.capture.push_back(std::filesystem::absolute(*v, ec).string());
        } else if (a == "--needs-version") {
            auto v = value(a);
            if (!v) return std::nullopt;
            o.needs_version = *v;
        } else if (a == "--report") {
            auto v = value(a);
            if (!v) return std::nullopt;
            std::error_code ec;
            o.report = std::filesystem::absolute(*v, ec).string();
        } else if (a == "--from") {
            auto v = value(a);
            if (!v) return std::nullopt;
            o.from = *v;
        } else if (a == "--keep-going") {
            o.keep_going = true;
        } else if (a == "--no-cache") {
            o.no_cache = true;
        } else if (a == "--force-large") {
            o.force_large = true;
        } else if (a == "--each") {
            auto v = value(a);
            if (!v) return std::nullopt;
            o.each_glob = *v;
        } else if (!a.empty() && a[0] == '-') {
            *error = "unknown option '" + a + "'";
            return std::nullopt;
        } else {
            break; // the command starts here (no "--")
        }
    }
    o.argv.assign(args.begin() + static_cast<std::ptrdiff_t>(std::min(i, args.size())), args.end());
    if (o.remote_only && o.local_only) {
        *error = "--remote-only and --local-only exclude each other";
        return std::nullopt;
    }
    if (o.from) {
        if (!o.argv.empty() || o.each_glob) {
            *error = "--from takes the whole run from the file: no command or --each";
            return std::nullopt;
        }
        return o;
    }
    if (o.argv.empty()) {
        *error = "missing command (zm run [options] -- COMMAND ...)";
        return std::nullopt;
    }
    if (!o.capture.empty() && o.env != "capture") {
        *error = "--capture needs --env capture";
        return std::nullopt;
    }
    return o;
}

Watched WatchRun(Client& client, const std::string& run_id, bool stream_logs, bool item_lines, std::string* error) {
    Watched w;
    CancelOnInterrupt guard(client.stub(), run_id);
    grpc::ClientContext ctx;
    v1::WatchRequest req;
    req.set_run_id(run_id);
    req.set_from_start(true);
    auto reader = client.stub().WatchRun(&ctx, req);
    v1::RunEvent ev;
    while (reader->Read(&ev)) {
        if (ev.has_log() && stream_logs) {
            WriteAll(ev.log().fd() == 2 ? 2 : 1, ev.log().chunk());
        } else if (ev.has_result()) {
            const auto& r = ev.result();
            w.results[r.id()] = r;
            if (item_lines) {
                std::string where;
                if (r.attempts_size() > 0) where = r.attempts(r.attempts_size() - 1).hostname();
                if (r.state() == v1::ITEM_SUCCEEDED) {
                    std::cerr << "  done    " << r.id() << (r.cache().hit() ? "  (cached)" : "  on " + where) << "\n";
                } else {
                    std::cerr << "  " << (r.state() == v1::ITEM_CANCELLED ? "cancel  " : "FAILED  ") << Describe(r.id(), r)
                              << "\n";
                }
            }
        } else if (ev.has_update() && ev.update().state() == v1::ITEM_QUEUED && !ev.update().detail().empty()) {
            std::cerr << "zm: " << ev.update().id() << ": " << ev.update().detail() << "\n";
        } else if (ev.has_notice()) {
            std::cerr << "zm: " << ev.notice().text() << "\n";
        } else if (ev.has_finished()) {
            w.status = ev.finished();
            w.finished = true;
        }
    }
    grpc::Status s = reader->Finish();
    if (!s.ok() && !w.finished) *error = s.error_message();
    return w;
}

namespace {

int RunFromFile(Client& client, const RunOptions& opts) {
    auto text = ReadFile(*opts.from);
    if (!text) {
        std::cerr << "zm: cannot read " << *opts.from << "\n";
        return 1;
    }
    v1::RunSpec spec;
    google::protobuf::util::JsonParseOptions jopts;
    if (auto st = google::protobuf::util::JsonStringToMessage(*text, &spec, jopts); !st.ok()) {
        std::cerr << "zm: " << *opts.from << " is not a valid run: " << st.ToString() << "\n";
        return 1;
    }
    std::error_code ec;
    const auto base = std::filesystem::absolute(*opts.from, ec).parent_path();
    if (spec.workspace().hash().empty() && !spec.workspace_path().empty()) {
        std::filesystem::path ws = spec.workspace_path();
        if (ws.is_relative()) ws = base / ws;
        grpc::ClientContext ctx;
        v1::SnapshotRequest req;
        req.set_path(ws.lexically_normal().string());
        req.set_force_large(opts.force_large);
        v1::SnapshotReply snap;
        grpc::Status s = client.stub().Snapshot(&ctx, req, &snap);
        if (!s.ok()) return ReportRpcError("snapshot failed", s);
        *spec.mutable_workspace() = snap.root();
        spec.set_workspace_path(req.path());
    }
    if (!opts.report.empty()) spec.set_report_path(opts.report);
    if (spec.name().empty()) spec.set_name("zm run --from");
    v1::RunRef ref;
    {
        grpc::ClientContext ctx;
        grpc::Status s = client.stub().SubmitRun(&ctx, spec, &ref);
        if (!s.ok()) return ReportRpcError("submit failed", s);
    }
    std::cerr << "zm: run " << ref.run_id() << ": " << spec.items_size() << " item(s)\n";
    std::string err;
    Watched w = WatchRun(client, ref.run_id(), spec.stream_logs(), true, &err);
    if (!w.finished) {
        std::cerr << "zm: lost the run: " << err << " (zm attach " << ref.run_id() << ")\n";
        return 1;
    }
    std::cerr << "zm: " << w.status.succeeded() << " succeeded (" << w.status.cached() << " cached), " << w.status.failed()
              << " failed, " << w.status.cancelled() << " cancelled\n";
    return w.status.failed() > 0 || w.status.cancelled() > 0 ? 1 : 0;
}

} // namespace

std::optional<v1::CaptureRequest> CaptureRequestFor(const std::vector<std::string>& probe,
                                                    const std::vector<std::string>& paths, std::string* error) {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    v1::CaptureRequest req;
    req.set_cwd(cwd.string());
    for (const auto& p : paths) req.add_paths(std::filesystem::absolute(p, ec).lexically_normal().string());
    if (!probe.empty()) {
        std::string program = probe.front();
        if (program.find('/') != std::string::npos) program = std::filesystem::absolute(program, ec).string();
        const std::string found = exec::FindOnPath(program);
        if (found.empty()) {
            *error = "'" + probe.front() + "' is not an executable program on PATH";
            return std::nullopt;
        }
        auto* pr = req.add_probes();
        pr->add_argv(found);
        for (size_t i = 1; i < probe.size(); ++i) pr->add_argv(probe[i]);
    }
    return req;
}

int RunCommand(Client& client, const RunOptions& opts) {
    if (opts.from) return RunFromFile(client, opts);
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (ec) {
        std::cerr << "zm: cannot determine the current directory: " << ec.message() << "\n";
        return 1;
    }

    std::vector<std::string> each_matches;
    if (opts.each_glob) {
        std::string err;
        if (!ExpandEachGlob(*opts.each_glob, cwd, &each_matches, &err)) {
            std::cerr << "zm: " << err << "\n";
            return 1;
        }
    }

    v1::SnapshotReply snap;
    {
        grpc::ClientContext ctx;
        v1::SnapshotRequest req;
        req.set_path(cwd.string());
        req.set_force_large(opts.force_large);
        grpc::Status s = client.stub().Snapshot(&ctx, req, &snap);
        if (!s.ok()) return ReportRpcError("snapshot failed", s);
    }
    if (!snap.warning().empty()) std::cerr << "zm: " << snap.warning() << "\n";

    v1::RunSpec spec;
    spec.set_name(opts.each_glob ? "zm run --each" : "zm run");
    *spec.mutable_workspace() = snap.root();
    spec.set_workspace_path(cwd.string());
    spec.set_stream_logs(true);
    if (!opts.report.empty()) spec.set_report_path(opts.report);
    auto* policy = spec.mutable_policy();
    policy->set_keep_going(opts.keep_going || opts.each_glob.has_value());
    if (opts.no_cache) policy->set_cache(false);

    v1::Stage* stage = spec.add_stages();
    if (opts.cores > 0) stage->mutable_resources()->set_cores(opts.cores);
    if (opts.mem_bytes > 0) stage->mutable_resources()->set_mem_bytes(opts.mem_bytes);
    if (opts.gpus > 0) stage->mutable_resources()->set_gpus(opts.gpus);
    stage->set_where(opts.remote_only ? v1::REMOTE_ONLY : opts.local_only ? v1::LOCAL_ONLY : v1::ANYWHERE);
    stage->set_kind(std::filesystem::path(opts.argv.front()).filename().string());
    if (opts.env == "capture") {
        std::string error;
        auto req = CaptureRequestFor({opts.argv.front(), "--version"}, opts.capture, &error);
        if (!req) {
            std::cerr << "zm: " << error << "\n";
            return 1;
        }
        grpc::ClientContext ctx;
        v1::CaptureReply cap;
        grpc::Status s = client.stub().CaptureEnv(&ctx, *req, &cap);
        if (!s.ok()) return ReportRpcError("capturing the environment failed", s);
        if (!cap.note().empty()) std::cerr << "zm: " << cap.note() << "\n";
        *stage->mutable_env()->mutable_tree() = cap.tree();
    } else if (opts.env.rfind("sha256:", 0) == 0) {
        stage->mutable_env()->mutable_tree()->set_hash(opts.env.substr(7));
    } else {
        auto* program = stage->mutable_env()->add_programs();
        program->set_name(opts.argv.front());
        program->set_version(opts.needs_version);
    }

    struct Slot {
        std::string id;
        std::string label;
    };
    std::vector<Slot> slots;
    auto add_item = [&](const std::string& id, const std::string& label, const std::vector<std::string>& argv) {
        v1::Item* it = spec.add_items();
        it->set_id(id);
        for (const auto& a : argv) it->add_argv(a);
        for (const auto& o : opts.outputs) {
            auto* out = it->add_outputs();
            out->set_path(o);
            out->set_dest((cwd / o).lexically_normal().string());
        }
        if (opts.outputs.empty()) it->set_changed_dest(cwd.string());
        slots.push_back({id, label});
    };
    if (each_matches.empty()) {
        add_item("run", "run", opts.argv);
    } else {
        for (size_t i = 0; i < each_matches.size(); ++i) {
            std::vector<std::string> argv;
            for (const auto& a : opts.argv) argv.push_back(ReplaceBraces(a, each_matches[i]));
            add_item("each-" + std::to_string(i), each_matches[i], argv);
        }
    }

    v1::RunRef ref;
    {
        grpc::ClientContext ctx;
        grpc::Status s = client.stub().SubmitRun(&ctx, spec, &ref);
        if (!s.ok()) return ReportRpcError("submit failed", s);
    }
    std::string err;
    Watched w = WatchRun(client, ref.run_id(), /*stream_logs=*/true, /*item_lines=*/false, &err);
    if (!w.finished) {
        std::cerr << "zm: lost the run: " << err << " (zm attach " << ref.run_id() << ")\n";
        return 1;
    }

    const bool multi = slots.size() > 1 || opts.each_glob.has_value();
    std::vector<std::string> failures;
    int exit_code = 0;
    for (const auto& slot : slots) {
        auto it = w.results.find(slot.id);
        if (it == w.results.end()) {
            failures.push_back(slot.label + ": no result");
            if (exit_code == 0) exit_code = 1;
            continue;
        }
        const v1::ItemResult& r = it->second;
        if (r.cache().hit()) ReplayLogs(client, r);
        const int code = ExitCodeOf(r);
        if (code == 0) continue;
        if (!multi) {
            // The command's own exit code speaks for itself; anything else needs words.
            if (r.failure().kind() != v1::FAILURE_EXIT) std::cerr << "zm: " << Describe("the command", r) << "\n";
            return code;
        }
        failures.push_back(Describe(slot.label, r));
        if (exit_code == 0) exit_code = code;
    }
    if (!failures.empty()) {
        std::cerr << "zm: " << failures.size() << " item(s) failed:\n";
        for (const auto& f : failures) std::cerr << "  " << f << "\n";
    }
    return exit_code;
}

} // namespace zeromesh::cli
