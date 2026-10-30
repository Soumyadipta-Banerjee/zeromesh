#include "node/departure.hpp"

#include "store/store.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace zeromesh::node {

namespace {

constexpr std::string_view kMarker = "[freeze-first ttl_ms=";

// Starts `sh -c cmd` in its own process group. With out_fd, the child's stdout is a pipe whose
// read end is returned there. Returns the pid, or -1.
pid_t Spawn(const std::string& cmd, int* out_fd) {
    int fds[2] = {-1, -1};
    if (out_fd && pipe2(fds, O_CLOEXEC) != 0) return -1;
    const pid_t pid = fork();
    if (pid < 0) {
        if (out_fd) {
            close(fds[0]);
            close(fds[1]);
        }
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (!out_fd) dup2(devnull, STDOUT_FILENO);
        }
        if (out_fd) dup2(fds[1], STDOUT_FILENO);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    setpgid(pid, pid); // also from the parent, so the group exists before the first kill(-pid)
    if (out_fd) {
        close(fds[1]);
        *out_fd = fds[0];
    }
    return pid;
}

void KillGroup(pid_t pid, int sig) {
    if (pid <= 0) return;
    kill(-pid, sig);
    kill(pid, sig);
    waitpid(pid, nullptr, 0);
}

bool OnPath(const char* tool) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        if (access((dir + "/" + tool).c_str(), X_OK) == 0) return true;
    }
    return false;
}

} // namespace

const char* DepartureName(v1::DepartureReason code) {
    switch (code) {
    case v1::DEPARTURE_UNPLUG:
        return "unplug";
    case v1::DEPARTURE_BATTERY:
        return "battery";
    case v1::DEPARTURE_LID:
        return "lid";
    case v1::DEPARTURE_OWNER:
        return "owner";
    case v1::DEPARTURE_SUSPEND:
        return "suspend";
    default:
        return "departure";
    }
}

std::string FormatDepartureReason(v1::DepartureReason code, uint32_t ttl_ms, std::string_view detail) {
    std::string out = DepartureName(code);
    if (ttl_ms > 0) {
        out += ' ';
        out += kMarker;
        out += std::to_string(ttl_ms);
        out += ']';
    }
    if (!detail.empty()) {
        out += ' ';
        out += detail;
    }
    return out;
}

std::optional<ParsedDeparture> ParseDepartureReason(std::string_view reason) {
    const size_t at = reason.find(kMarker);
    if (at == std::string_view::npos) return std::nullopt;
    const size_t num = at + kMarker.size();
    const size_t close = reason.find(']', num);
    if (close == std::string_view::npos) return std::nullopt;
    uint32_t ttl = 0;
    const char* b = reason.data() + num;
    const char* e = reason.data() + close;
    auto [ptr, ec] = std::from_chars(b, e, ttl);
    if (ec != std::errc{} || ptr != e || ttl == 0) return std::nullopt;

    ParsedDeparture p;
    p.ttl_ms = ttl;
    const std::string_view name = reason.substr(0, reason.find_first_of(" ["));
    for (v1::DepartureReason c : {v1::DEPARTURE_UNPLUG, v1::DEPARTURE_BATTERY, v1::DEPARTURE_LID,
                                  v1::DEPARTURE_OWNER, v1::DEPARTURE_SUSPEND}) {
        if (name == DepartureName(c)) p.code = c;
    }
    return p;
}

// ---- RuntimeHistory -------------------------------------------------------------------------

RuntimeHistory::RuntimeHistory(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) return;
    std::ifstream in(file_);
    std::string line;
    size_t lines = 0;
    while (std::getline(in, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        int64_t ms = 0;
        const char* b = line.data() + tab + 1;
        const char* e = line.data() + line.size();
        if (std::from_chars(b, e, ms).ec != std::errc{}) continue;
        RecordLocked(line.substr(0, tab), ms);
        ++lines;
    }
    // Compact a file that has grown much larger than what it says.
    if (lines > 4 * expected_ms_.size() + 1000) {
        std::ofstream out(file_, std::ios::trunc);
        for (const auto& [key, ms] : expected_ms_) out << key << '\t' << ms << '\n';
    }
}

std::string RuntimeHistory::Key(const v1::TaskSpec& spec) {
    std::string s;
    for (const auto& a : spec.argv()) {
        s += a;
        s += '\x1f';
    }
    s += '\x1e';
    std::map<std::string, std::string> env(spec.env().begin(), spec.env().end());
    for (const auto& [k, v] : env) {
        s += k;
        s += '=';
        s += v;
        s += '\x1f';
    }
    s += '\x1e';
    s += spec.env_spec().name().empty() ? "host" : spec.env_spec().name();
    return store::CasStore::ComputeDigest(s).hash;
}

void RuntimeHistory::RecordLocked(const std::string& key, int64_t duration_ms) {
    if (duration_ms < 0) return;
    auto it = expected_ms_.find(key);
    if (it == expected_ms_.end()) {
        expected_ms_[key] = duration_ms;
    } else {
        it->second = (it->second + duration_ms) / 2;
    }
}

void RuntimeHistory::Record(const v1::TaskSpec& spec, int64_t duration_ms) {
    if (duration_ms < 0) return;
    const std::string key = Key(spec);
    std::lock_guard lock(mu_);
    RecordLocked(key, duration_ms);
    if (!file_.empty()) {
        std::ofstream out(file_, std::ios::app);
        out << key << '\t' << duration_ms << '\n';
    }
}

std::optional<int64_t> RuntimeHistory::Expected(const v1::TaskSpec& spec) const {
    const std::string key = Key(spec);
    std::lock_guard lock(mu_);
    auto it = expected_ms_.find(key);
    if (it == expected_ms_.end()) return std::nullopt;
    return it->second;
}

// ---- the plan -------------------------------------------------------------------------------

DeparturePlan PlanDeparture(const std::vector<RunningTask>& tasks, std::chrono::milliseconds grace) {
    DeparturePlan plan;
    for (const auto& t : tasks) {
        if (!t.preemptible) {
            plan.keep.push_back(t.id);
        } else if (!t.expected_ms) {
            plan.freeze.push_back(t.id);
        } else {
            const int64_t remaining = std::max<int64_t>(0, *t.expected_ms - t.elapsed_ms);
            (remaining <= grace.count() ? plan.keep : plan.freeze).push_back(t.id);
        }
    }
    return plan;
}

// ---- signals --------------------------------------------------------------------------------

DepartureWatcher::DepartureWatcher(bool depart_on_lid, bool depart_on_unplug)
    : depart_on_lid_(depart_on_lid), depart_on_unplug_(depart_on_unplug) {}

std::optional<DepartureEvent> DepartureWatcher::Update(const SignalState& s) {
    v1::DepartureReason want = v1::DEPARTURE_UNSPECIFIED;
    if (s.sleep_pending) {
        want = v1::DEPARTURE_SUSPEND;
    } else if (depart_on_lid_ && s.lid_closed) {
        want = v1::DEPARTURE_LID;
    } else if (depart_on_unplug_ && !s.on_ac) {
        want = v1::DEPARTURE_UNPLUG;
    }
    if (want != v1::DEPARTURE_UNSPECIFIED && !active_) {
        active_ = true;
        reason_ = want;
        return DepartureEvent{true, want};
    }
    if (want == v1::DEPARTURE_UNSPECIFIED && active_) {
        active_ = false;
        return DepartureEvent{false, reason_};
    }
    return std::nullopt;
}

std::optional<bool> ReadLidClosed(const std::filesystem::path& sysroot) {
    const auto dir = sysroot / "proc/acpi/button/lid";
    std::error_code ec;
    std::optional<bool> closed;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
        std::ifstream in(it->path() / "state");
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.find("closed") != std::string::npos) return true;
        if (text.find("open") != std::string::npos) closed = false;
    }
    return closed;
}

std::optional<bool> ParsePrepareForSleep(std::string_view line) {
    if (line.find("\"PrepareForSleep\"") == std::string_view::npos) return std::nullopt;
    const size_t data = line.find("\"data\"");
    if (data == std::string_view::npos) return std::nullopt;
    size_t open = line.find('[', data);
    if (open == std::string_view::npos) return std::nullopt;
    ++open;
    while (open < line.size() && line[open] == ' ') ++open;
    const std::string_view rest = line.substr(open);
    if (rest.rfind("true", 0) == 0) return true;
    if (rest.rfind("false", 0) == 0) return false;
    return std::nullopt;
}

// ---- LogindMonitor --------------------------------------------------------------------------

LogindMonitor::Config LogindMonitor::DefaultConfig() {
    Config c;
    if (OnPath("busctl")) c.monitor_cmd = "exec busctl --system --json=short monitor org.freedesktop.login1";
    if (OnPath("systemd-inhibit")) {
        c.inhibit_cmd =
            "exec systemd-inhibit --what=sleep --mode=delay --who=zmd "
            "--why='Freezing donated work before sleep' sleep infinity";
    }
    return c;
}

bool LogindMonitor::Available() {
    return OnPath("busctl");
}

LogindMonitor::LogindMonitor(Config cfg, Callback cb) : cfg_(std::move(cfg)), cb_(std::move(cb)) {}

LogindMonitor::~LogindMonitor() {
    Stop();
}

void LogindMonitor::Start() {
    if (cfg_.monitor_cmd.empty() || thread_.joinable()) return;
    int fd = -1;
    const pid_t pid = Spawn(cfg_.monitor_cmd, &fd);
    if (pid < 0) return;
    {
        std::lock_guard lock(mu_);
        monitor_pid_ = pid;
    }
    TakeInhibitor();
    thread_ = std::jthread([this, fd](std::stop_token st) {
        Loop(st, fd);
    });
}

void LogindMonitor::Stop() {
    if (thread_.joinable()) thread_.request_stop();
    pid_t pid = -1;
    {
        std::lock_guard lock(mu_);
        pid = monitor_pid_;
        monitor_pid_ = -1;
    }
    KillGroup(pid, SIGKILL);
    if (thread_.joinable()) thread_.join();
    ReleaseInhibitor();
}

void LogindMonitor::TakeInhibitor() {
    if (cfg_.inhibit_cmd.empty()) return;
    std::lock_guard lock(mu_);
    if (inhibitor_pid_ > 0) return;
    inhibitor_pid_ = Spawn(cfg_.inhibit_cmd, nullptr);
}

void LogindMonitor::ReleaseInhibitor() {
    pid_t pid = -1;
    {
        std::lock_guard lock(mu_);
        pid = inhibitor_pid_;
        inhibitor_pid_ = -1;
    }
    KillGroup(pid, SIGTERM);
}

void LogindMonitor::Loop(std::stop_token st, int fd) {
    std::string buf;
    char chunk[4096];
    while (!st.stop_requested()) {
        pollfd p{fd, POLLIN, 0};
        const int r = poll(&p, 1, 200);
        if (r <= 0) continue;
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) break; // the monitor exited
        buf.append(chunk, static_cast<size_t>(n));
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            const auto sleeping = ParsePrepareForSleep(line);
            if (!sleeping) continue;
            if (*sleeping) {
                if (cb_) cb_(true);
                ReleaseInhibitor(); // the work is frozen: let the system sleep
            } else {
                TakeInhibitor(); // awake again: guard the next sleep
                if (cb_) cb_(false);
            }
        }
    }
    close(fd);
}

} // namespace zeromesh::node
