#include "cli/tui.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace zeromesh::cli {

namespace {

std::string Gib(int64_t bytes) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0) << " GB";
    return ss.str();
}

std::string Pct(double v) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << v << "%";
    return ss.str();
}

std::string Power(const v1::PeerInfo& p) {
    if (p.battery_pct() < 0) return "AC";
    return std::to_string(p.battery_pct()) + "%" + (p.on_ac() ? " (AC)" : "");
}

std::string Clip(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n - 3) + "..." : s; }

const char* StateName(v1::TaskState s) {
    switch (s) {
        case v1::QUEUED: return "QUEUED";
        case v1::PLACED: return "PLACED";
        case v1::RUNNING: return "RUNNING";
        case v1::SUCCEEDED: return "DONE";
        case v1::FAILED: return "FAILED";
        case v1::CANCELLED: return "CANCEL";
        default: return "?";
    }
}

bool IsWorking(v1::TaskState s) {
    return s == v1::QUEUED || s == v1::PLACED || s == v1::RUNNING;
}

std::string ArgvJoin(const v1::ActiveTask& t, size_t max_len = 48) {
    std::ostringstream ss;
    for (int i = 0; i < t.argv_size(); ++i) {
        if (i) ss << ' ';
        ss << t.argv(i);
    }
    return Clip(ss.str(), max_len);
}

// worker_id is the node that is running (or ran) the task. Resolve it to a hostname.
std::string Where(const v1::ActiveTask& t, const v1::PeerList* peers) {
    if (t.role() == "executing") return "here";
    const std::string& id = t.worker_id();
    if (id.empty()) return "-";
    if (peers != nullptr) {
        for (const auto& p : peers->peers()) {
            if (p.node_id() == id) return p.is_self() ? "here" : p.hostname();
        }
    }
    return id.size() <= 8 ? id : id.substr(0, 8);
}

int TermRows() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 3) return ws.ws_row;
    return 24;
}

int TermCols() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20) return ws.ws_col;
    return 80;
}

// Keep CSI sequences, count only visible columns, always end with a reset.
std::string ClipAnsi(const std::string& s, int cols) {
    if (cols < 1) return {};
    std::string out;
    int vis = 0;
    bool cut = false;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && !(s[j] >= '@' && s[j] <= '~')) ++j;
            if (j < s.size()) ++j;
            out.append(s, i, j - i);
            i = j;
            continue;
        }
        if (vis >= cols) {
            cut = true;
            break;
        }
        out.push_back(s[i]);
        ++vis;
        ++i;
    }
    if (cut && cols > 3) {
        while (vis > cols - 3 && !out.empty() && out.back() != '\033') {
            out.pop_back();
            --vis;
        }
        out += "...";
    }
    out += "\033[0m";
    return out;
}

// Never emit more lines than the terminal, or the pane scrolls and the next
// refresh prints a second copy instead of replacing the first.
std::string FitScreen(const std::string& body, int rows, int cols) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : body) {
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) lines.push_back(std::move(cur));

    const int body_rows = std::max(1, rows - 1);
    if (static_cast<int>(lines.size()) > body_rows) {
        lines.resize(static_cast<size_t>(body_rows - 1));
        lines.push_back("… more (pane too short)");
    }
    std::ostringstream os;
    for (const auto& ln : lines) os << ClipAnsi(ln, cols) << "\033[K\n";
    for (int i = static_cast<int>(lines.size()); i < rows - 1; ++i) os << "\033[K\n";
    os << ClipAnsi("q / Ctrl-C to quit", cols) << "\033[K";
    return os.str();
}

struct RawTty {
    bool on{false};
    termios saved{};
    RawTty() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) != 0) return;
        termios raw = saved;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        on = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    ~RawTty() {
        if (on) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    }
};

} // namespace

void RenderStatus(const v1::PeerList& peers, std::ostream& os, bool color) {
    const char* bold = color ? "\033[1m" : "";
    const char* dim = color ? "\033[90m" : "";
    const char* green = color ? "\033[32m" : "";
    const char* yellow = color ? "\033[33m" : "";
    const char* reset = color ? "\033[0m" : "";

    const v1::PeerInfo* self = nullptr;
    for (const auto& p : peers.peers()) {
        if (p.is_self()) self = &p;
    }

    os << bold << "ZeroMesh status" << reset << "\n\n";
    if (self != nullptr) {
        os << bold << "This node" << reset << "\n"
           << "  Node ID:   " << self->node_id() << "\n"
           << "  Hostname:  " << self->hostname() << " (" << self->arch() << ")\n"
           << "  Cores:     " << self->cores_free() << " free of " << self->cores_total() << "\n"
           << "  Memory:    " << Gib(self->mem_available()) << " available\n"
           << "  CPU load:  " << Pct(self->cpu_load_pct()) << "\n"
           << "  Power:     " << Power(*self) << "\n"
           << "  State:     " << self->state() << "\n\n";
    }

    os << bold << "Peers" << reset << "\n";
    int others = 0;
    for (const auto& p : peers.peers()) others += p.is_self() ? 0 : 1;
    if (others == 0) {
        os << "  " << dim << "(no peers yet)" << reset << "\n";
        return;
    }
    os << "  " << std::left << std::setw(10) << "NODE ID" << "  " << std::setw(24) << "HOST" << "  "
       << std::right << std::setw(9) << "CORES" << "  " << std::setw(9) << "MEM" << "  " << std::setw(7) << "CPU"
       << "  " << std::left << std::setw(10) << "STATE" << "  ADMITTED\n";
    for (const auto& p : peers.peers()) {
        if (p.is_self()) continue;
        std::string host = p.hostname() + " (" + p.address() + ":" + std::to_string(p.node_port()) + ")";
        std::string cores = std::to_string(p.cores_free()) + "/" + std::to_string(p.cores_total());
        const char* state_color = p.state() == "active" ? green : yellow;
        std::string id8 = p.node_id().size() <= 8 ? p.node_id() : p.node_id().substr(0, 8);
        os << "  " << std::left << std::setw(10) << id8 << "  " << std::setw(24) << Clip(host, 24)
           << "  " << std::right << std::setw(9) << cores << "  " << std::setw(9) << Gib(p.mem_available()) << "  "
           << std::setw(7) << Pct(p.cpu_load_pct()) << "  " << state_color << std::left << std::setw(10)
           << p.state() << reset << "  " << (p.admitted() ? "yes" : "no") << "\n";
    }
}

void RenderActivity(const v1::ActivitySnapshot& activity, std::ostream& os, bool color, const v1::PeerList* peers) {
    const char* bold = color ? "\033[1m" : "";
    const char* dim = color ? "\033[90m" : "";
    const char* cyan = color ? "\033[36m" : "";
    const char* green = color ? "\033[32m" : "";
    const char* yellow = color ? "\033[33m" : "";
    const char* red = color ? "\033[31m" : "";
    const char* reset = color ? "\033[0m" : "";

    int working = 0, done = 0, failed = 0;
    for (const auto& t : activity.tasks()) {
        if (IsWorking(t.state())) ++working;
        else if (t.state() == v1::SUCCEEDED) ++done;
        else ++failed;
    }

    os << bold << "Tasks" << reset;
    if (activity.tasks_size() > 0) {
        os << "  " << dim << working << " in flight · " << done << " done";
        if (failed > 0) os << " · " << failed << " failed";
        os << reset;
    }
    os << "\n";
    if (activity.tasks_size() == 0) {
        os << "  " << dim << "(idle — no tasks yet)" << reset << "\n";
        return;
    }

    os << "  " << std::left << std::setw(10) << "ROLE" << " " << std::setw(8) << "STATE" << " " << std::setw(16)
       << "WHERE" << " " << std::setw(16) << "TASK" << "  COMMAND\n";

    for (const auto& t : activity.tasks()) {
        const char* role_color = t.role() == "executing" ? green : cyan;
        const char* state_color = IsWorking(t.state())        ? yellow
                                  : t.state() == v1::SUCCEEDED ? green
                                  : t.state() == v1::FAILED    ? red
                                                               : dim;
        std::string name = t.task_name();
        if (name.size() > 16) name = name.substr(0, 13) + "...";
        std::string where = Where(t, peers);
        if (where.size() > 16) where = where.substr(0, 13) + "...";
        os << "  " << role_color << std::left << std::setw(10) << t.role() << reset << " " << state_color
           << std::setw(8) << StateName(t.state()) << reset << " " << std::setw(16) << where << " " << std::setw(16)
           << name << "  " << ArgvJoin(t) << "\n";
    }
}

void RenderMonitor(const v1::PeerList& peers, const v1::ActivitySnapshot& activity, std::ostream& os, bool color) {
    const char* bold = color ? "\033[1m" : "";
    const char* dim = color ? "\033[90m" : "";
    const char* green = color ? "\033[32m" : "";
    const char* reset = color ? "\033[0m" : "";

    const v1::PeerInfo* self = nullptr;
    for (const auto& p : peers.peers()) {
        if (p.is_self()) self = &p;
    }

    os << bold << "ZeroMesh" << reset;
    if (self != nullptr) {
        os << "  " << self->hostname() << "  " << self->cores_free() << "/" << self->cores_total() << " cores  "
           << Pct(self->cpu_load_pct()) << " cpu  " << Gib(self->mem_available());
    }
    os << "\n";

    int others = 0;
    for (const auto& p : peers.peers()) {
        if (p.is_self()) continue;
        ++others;
        os << "  peer " << (p.admitted() ? green : dim) << p.hostname() << reset << "  " << p.cores_free() << "/"
           << p.cores_total() << " cores  " << (p.admitted() ? "admitted" : "waiting") << "\n";
    }
    if (others == 0) os << "  " << dim << "(no peers yet)" << reset << "\n";
    os << "\n";
    RenderActivity(activity, os, color, &peers);
}

int RunMonitor(const PeerFetcher& fetch_peers, const ActivityFetcher& fetch_activity,
               const std::atomic<bool>& stop) {
    const bool tty = isatty(STDOUT_FILENO) != 0;
    bool read_stdin = true;
    int rc = 0;
    RawTty raw;
    // Alternate screen: redraws replace the pane instead of scrolling a new copy.
    if (tty) std::cout << "\033[?1049h\033[?25l\033[2J\033[H" << std::flush;

    while (!stop.load()) {
        std::string error;
        bool unavailable = false;
        auto peers = fetch_peers(&error, &unavailable);
        if (!peers && unavailable) {
            rc = 2;
            break;
        }
        v1::ActivitySnapshot activity;
        if (peers) {
            auto act = fetch_activity(&error, &unavailable);
            if (!act && unavailable) {
                rc = 2;
                break;
            }
            if (act) activity = *act;
        }

        std::ostringstream frame;
        if (peers) RenderMonitor(*peers, activity, frame, tty);
        else frame << "error: " << error << "\n";

        if (tty) {
            std::cout << "\033[H\033[2J" << FitScreen(frame.str(), TermRows(), TermCols()) << std::flush;
        } else {
            std::cout << frame.str() << "\n---\n" << std::flush;
        }

        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        int ret = poll(&pfd, read_stdin ? 1 : 0, 1000);
        if (ret < 0 && errno == EINTR) continue;
        if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char ch = 0;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0) read_stdin = false;
            else if (ch == 'q' || ch == 'Q' || ch == 27) break;
        }
    }
    if (tty) std::cout << "\033[?25h\033[?1049l" << std::flush;
    else std::cout << "\n" << std::flush;
    return rc;
}

} // namespace zeromesh::cli
