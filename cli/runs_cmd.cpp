#include "cli/runs_cmd.hpp"

#include "cli/run_cmd.hpp"
#include "cli/tui.hpp"

#include <google/protobuf/util/json_util.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>

namespace zeromesh::cli {

const char* const kRunsUsage =
    "  runs                  Runs submitted here, running first, then the history\n"
    "  attach RUN            Follow a run until it ends (Ctrl-C cancels it)\n"
    "  cancel RUN            Cancel a run; finished items stay\n"
    "  report RUN [--json]   Show a run's report (items, attempts, cache, failures)\n"
    "  resume RUN            Run a past run again; finished items come from the cache\n"
    "  cache explain RUN ITEM\n"
    "                        Why an item hit or missed the cache\n"
    "  cache forget (--run RUN | --all) [--team]\n"
    "                        Drop cache entries, here (and on the team's nodes)\n"
    "  env capture [--path P]... -- CMD...\n"
    "                        Capture the software CMD uses into an environment tree\n"
    "  doctor                Check this node: sandbox, cgroups, FUSE, team, peers\n"
    "  apps                  Installed adapters (zm-* on PATH); `zm NAME` runs zm-NAME\n";

namespace {

std::string When(int64_t ms) {
    if (ms <= 0) return "-";
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%m-%d %H:%M");
    return ss.str();
}

std::string Seconds(int64_t ms) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(ms < 10000 ? 2 : 0) << static_cast<double>(ms) / 1000.0 << "s";
    return ss.str();
}

std::optional<v1::Report> GetReport(Client& client, const std::string& run_id, int* rc) {
    grpc::ClientContext ctx;
    v1::RunRef ref;
    ref.set_run_id(run_id);
    v1::Report rep;
    grpc::Status s = client.stub().GetReport(&ctx, ref, &rep);
    if (!s.ok()) {
        *rc = ReportRpcError("no report for run " + run_id, s);
        return std::nullopt;
    }
    return rep;
}

const char* StateWord(v1::ItemState s) {
    switch (s) {
    case v1::ITEM_WAITING:
        return "waiting";
    case v1::ITEM_QUEUED:
        return "queued";
    case v1::ITEM_RUNNING:
        return "running";
    case v1::ITEM_SUCCEEDED:
        return "done";
    case v1::ITEM_FAILED:
        return "FAILED";
    case v1::ITEM_CANCELLED:
        return "cancelled";
    default:
        return "?";
    }
}

} // namespace

int RunsCommand(Client& client) {
    grpc::ClientContext ctx;
    v1::RunList list;
    grpc::Status s = client.stub().ListRuns(&ctx, v1::Empty{}, &list);
    if (!s.ok()) return ReportRpcError("cannot list runs", s);
    if (list.runs_size() == 0) {
        std::cout << "no runs yet\n";
        return 0;
    }
    v1::ActivitySnapshot as_activity;
    *as_activity.mutable_runs() = list.runs();
    std::cout << std::left << "  " << std::setw(12) << "RUN" << " " << std::setw(14) << "NAME" << " " << std::setw(20)
              << "PROGRESS" << "  ITEMS\n";
    RenderActivity(as_activity, std::cout, isatty(STDOUT_FILENO) != 0);
    return 0;
}

int AttachCommand(Client& client, const std::string& run_id) {
    std::string err;
    Watched w = WatchRun(client, run_id, false, true, &err);
    if (!w.finished) {
        std::cerr << "zm: " << (err.empty() ? "the run did not finish" : err) << "\n";
        return 1;
    }
    std::cerr << "zm: run " << run_id << ": " << w.status.succeeded() << " succeeded (" << w.status.cached()
              << " cached), " << w.status.failed() << " failed, " << w.status.cancelled() << " cancelled\n";
    return w.status.failed() > 0 || w.status.cancelled() > 0 ? 1 : 0;
}

int CancelCommand(Client& client, const std::string& run_id) {
    grpc::ClientContext ctx;
    v1::RunRef ref;
    ref.set_run_id(run_id);
    v1::Empty empty;
    grpc::Status s = client.stub().CancelRun(&ctx, ref, &empty);
    if (!s.ok()) return ReportRpcError("cannot cancel run " + run_id, s);
    std::cout << "cancelling run " << run_id << "\n";
    return 0;
}

int ReportCommand(Client& client, const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "zm report: which run? (zm runs lists them)\n";
        return 1;
    }
    const bool json = args.size() > 1 && args[1] == "--json";
    int rc = 0;
    auto rep = GetReport(client, args[0], &rc);
    if (!rep) return rc;
    if (json) {
        std::string out;
        google::protobuf::util::JsonPrintOptions opts;
        opts.add_whitespace = true;
        opts.preserve_proto_field_names = true;
        (void)google::protobuf::util::MessageToJsonString(*rep, &out, opts);
        std::cout << out << "\n";
        return 0;
    }
    const auto& r = rep->run();
    std::cout << "run " << r.run_id() << "  " << r.name() << "  started " << When(r.started_ms()) << "\n"
              << "  " << r.succeeded() << " succeeded (" << r.cached() << " cached), " << r.failed() << " failed, "
              << r.cancelled() << " cancelled, " << r.running() + r.queued() << " unfinished\n"
              << "  core-seconds " << std::fixed << std::setprecision(1) << r.core_s() << ", inputs sent "
              << r.bytes_pushed() << " B (" << r.bytes_seeded() << " B seeded), outputs " << r.bytes_pulled() << " B\n";
    if (!r.ended_by().empty()) std::cout << "  ended by " << r.ended_by() << "\n";
    for (const auto& n : r.nodes()) {
        std::cout << "  node " << (n.hostname().empty() ? n.node().substr(0, 8) : n.hostname()) << ": " << n.items()
                  << " items";
        if (!n.excluded().empty()) std::cout << ", excluded: " << n.excluded();
        std::cout << "\n";
    }
    for (const auto& it : rep->items()) {
        std::cout << "  " << std::left << std::setw(9) << StateWord(it.state()) << " " << it.id();
        if (it.cache().hit()) {
            std::cout << "  cached";
        } else if (it.attempts_size() > 0) {
            const auto& a = it.attempts(it.attempts_size() - 1);
            std::cout << "  " << (a.hostname().empty() ? a.node().substr(0, 8) : a.hostname()) << " "
                      << Seconds(a.wall_ms());
            if (it.attempts_size() > 1) std::cout << " (" << it.attempts_size() << " attempts)";
        }
        if (it.has_failure()) std::cout << "  " << it.failure().message();
        std::cout << "\n";
    }
    return 0;
}

int ResumeCommand(Client& client, const std::string& run_id) {
    grpc::ClientContext ctx;
    v1::RunRef ref, fresh;
    ref.set_run_id(run_id);
    grpc::Status s = client.stub().ResumeRun(&ctx, ref, &fresh);
    if (!s.ok()) return ReportRpcError("cannot resume run " + run_id, s);
    std::cerr << "zm: resumed as run " << fresh.run_id() << "\n";
    return AttachCommand(client, fresh.run_id());
}

int CacheCommand(Client& client, const std::vector<std::string>& args) {
    if (!args.empty() && args[0] == "explain") {
        if (args.size() != 3) {
            std::cerr << "usage: zm cache explain RUN ITEM\n";
            return 1;
        }
        int rc = 0;
        auto rep = GetReport(client, args[1], &rc);
        if (!rep) return rc;
        for (const auto& it : rep->items()) {
            if (it.id() != args[2]) continue;
            const auto& c = it.cache();
            std::cout << it.id() << ": " << (c.hit() ? "hit" : "miss") << " — " << c.reason() << "\n";
            for (const auto& ch : c.changed()) std::cout << "  changed: " << ch << "\n";
            if (!c.key().empty()) std::cout << "  key " << c.key() << "\n";
            if (it.footprint_size() > 0) std::cout << "  read " << it.footprint_size() << " workspace files\n";
            return 0;
        }
        std::cerr << "zm: run " << args[1] << " has no item '" << args[2] << "'\n";
        return 1;
    }
    if (!args.empty() && args[0] == "forget") {
        v1::ForgetRequest req;
        for (size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--all") {
                req.set_all(true);
            } else if (args[i] == "--team") {
                req.set_team(true);
            } else if (args[i] == "--run" && i + 1 < args.size()) {
                req.set_run_id(args[++i]);
            } else {
                std::cerr << "usage: zm cache forget (--run RUN | --all) [--team]\n";
                return 1;
            }
        }
        if (!req.all() && req.run_id().empty()) {
            std::cerr << "usage: zm cache forget (--run RUN | --all) [--team]\n";
            return 1;
        }
        grpc::ClientContext ctx;
        v1::ForgetReply reply;
        grpc::Status s = client.stub().ForgetCache(&ctx, req, &reply);
        if (!s.ok()) return ReportRpcError("cannot forget", s);
        std::cout << "forgot " << reply.forgotten() << " entries" << (req.team() ? " (and told the team)" : "") << "\n";
        return 0;
    }
    std::cerr << "usage: zm cache explain RUN ITEM | zm cache forget (--run RUN | --all) [--team]\n";
    return 1;
}

int DoctorCommand(Client& client) {
    grpc::ClientContext ctx;
    v1::DoctorReply reply;
    grpc::Status s = client.stub().Doctor(&ctx, v1::Empty{}, &reply);
    if (!s.ok()) return ReportRpcError("doctor failed", s);
    const bool color = isatty(STDOUT_FILENO) != 0;
    int errors = 0;
    for (const auto& f : reply.findings()) {
        const char* mark = f.severity() == v1::SEVERITY_OK ? "ok   " : f.severity() == v1::SEVERITY_WARN ? "warn " : "ERROR";
        const char* c = !color ? "" : f.severity() == v1::SEVERITY_OK ? "\033[32m" : f.severity() == v1::SEVERITY_WARN ? "\033[33m" : "\033[31m";
        std::cout << c << mark << (color ? "\033[0m" : "") << "  " << std::left << std::setw(14) << f.check() << " "
                  << f.message() << "\n";
        if (!f.fix().empty()) std::cout << "                      fix: " << f.fix() << "\n";
        if (f.severity() == v1::SEVERITY_ERROR) ++errors;
    }
    return errors > 0 ? 1 : 0;
}

int EnvCommand(Client& client, const std::vector<std::string>& args) {
    if (args.empty() || args[0] != "capture") {
        std::cerr << "usage: zm env capture [--path P]... -- CMD [ARGS...]\n";
        return 1;
    }
    v1::CaptureRequest req;
    size_t i = 1;
    for (; i < args.size(); ++i) {
        if (args[i] == "--") {
            ++i;
            break;
        }
        if (args[i] == "--path" && i + 1 < args.size()) {
            std::error_code ec;
            req.add_paths(std::filesystem::absolute(args[++i], ec).string());
        } else {
            break;
        }
    }
    if (i < args.size()) {
        auto* probe = req.add_probes();
        for (; i < args.size(); ++i) probe->add_argv(args[i]);
    }
    if (req.probes_size() == 0 && req.paths_size() == 0) {
        std::cerr << "zm env capture: give a command to trace, --path, or both\n";
        return 1;
    }
    grpc::ClientContext ctx;
    v1::CaptureReply reply;
    grpc::Status s = client.stub().CaptureEnv(&ctx, req, &reply);
    if (!s.ok()) return ReportRpcError("capture failed", s);
    if (!reply.note().empty()) std::cerr << "zm: " << reply.note() << "\n";
    for (const auto& d : reply.denied()) std::cerr << "zm: kept out (secret): /" << d << "\n";
    std::cerr << "zm: " << reply.files() << " files, " << reply.bytes() << " bytes" << (reply.reused() ? " (unchanged)" : "")
              << "\n";
    std::cout << "sha256:" << reply.tree().hash() << "\n";
    return 0;
}

std::vector<std::string> InstalledApps() {
    std::set<std::string> names;
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.rfind("zm-", 0) != 0 || name.size() <= 3) continue;
            if (access(it->path().c_str(), X_OK) == 0) names.insert(name.substr(3));
        }
    }
    return {names.begin(), names.end()};
}

int AppsCommand() {
    auto apps = InstalledApps();
    if (apps.empty()) {
        std::cout << "no adapters installed (they are zm-NAME executables on PATH)\n";
        return 0;
    }
    for (const auto& a : apps) std::cout << "  " << a << "    (zm " << a << " ... runs zm-" << a << ")\n";
    return 0;
}

} // namespace zeromesh::cli
