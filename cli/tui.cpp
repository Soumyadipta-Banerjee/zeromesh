#include "cli/tui.hpp"

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
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

std::string FitScreen(const std::string& body, int rows, int cols, const std::string& footer) {
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
        lines.resize(static_cast<size_t>(body_rows));
    }
    std::ostringstream os;
    for (const auto& ln : lines) os << ClipAnsi(ln, cols) << "\033[K\n";
    for (int i = static_cast<int>(lines.size()); i < rows - 1; ++i) os << "\033[K\n";
    os << ClipAnsi(footer.empty() ? "q / Ctrl-C to quit" : footer, cols) << "\033[K";
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

void CountTasks(const v1::ActivitySnapshot& activity, int* live, int* done) {
    *live = 0;
    *done = 0;
    for (const auto& t : activity.tasks()) {
        if (IsWorking(t.state())) ++(*live);
        else ++(*done);
    }
}

std::vector<const v1::ActiveTask*> FilterTasks(const v1::ActivitySnapshot& activity, bool live_tab) {
    std::vector<const v1::ActiveTask*> out;
    out.reserve(static_cast<size_t>(activity.tasks_size()));
    for (const auto& t : activity.tasks()) {
        if (IsWorking(t.state()) == live_tab) out.push_back(&t);
    }
    return out;
}

void WriteTaskRow(const v1::ActiveTask& t, const v1::PeerList* peers, bool color, std::ostream& os) {
    const char* cyan = color ? "\033[36m" : "";
    const char* green = color ? "\033[32m" : "";
    const char* yellow = color ? "\033[33m" : "";
    const char* red = color ? "\033[31m" : "";
    const char* dim = color ? "\033[90m" : "";
    const char* reset = color ? "\033[0m" : "";

    const char* role_color = t.role() == "executing" ? green : cyan;
    const char* state_color = IsWorking(t.state())         ? yellow
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

// Read one keystroke / CSI sequence. Returns false on EOF / hangup.
// Special codes: negative ints for arrows / pages / mouse wheel / Tab handled as positive '\t'.
enum Key : int {
    KeyNone = 0,
    KeyQuit = 1,
    KeyUp = 2,
    KeyDown = 3,
    KeyPgUp = 4,
    KeyPgDn = 5,
    KeyTabLive = 6,  // switch to live (1)
    KeyTabDone = 7,  // switch to done (2)
    KeyToggleTab = 8,
    KeyClear = 9,
};

bool PollReadable(int timeout_ms) {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    int ret = poll(&pfd, 1, timeout_ms);
    return ret > 0 && (pfd.revents & POLLIN);
}

int ReadByte() {
    unsigned char ch = 0;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n <= 0) return -1;
    return static_cast<int>(ch);
}

int ParseEscSequence() {
    if (!PollReadable(40)) return KeyQuit; // bare Esc
    int c = ReadByte();
    if (c < 0) return KeyQuit;
    if (c == '[') {
        // CSI: arrows, PgUp/PgDn, or SGR mouse
        std::string seq;
        for (;;) {
            if (!PollReadable(40)) break;
            int b = ReadByte();
            if (b < 0) break;
            seq.push_back(static_cast<char>(b));
            if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~' || b == 'M' || b == 'm') break;
        }
        if (seq == "A") return KeyUp;
        if (seq == "B") return KeyDown;
        if (seq == "5~") return KeyPgUp;
        if (seq == "6~") return KeyPgDn;
        // SGR mouse: <btn;x;yM  (btn 64/65 = wheel)
        if (!seq.empty() && seq[0] == '<' && (seq.back() == 'M' || seq.back() == 'm')) {
            int btn = 0;
            if (sscanf(seq.c_str(), "<%d;", &btn) == 1) {
                if (btn == 64) return KeyUp;
                if (btn == 65) return KeyDown;
            }
        }
        return KeyNone;
    }
    if (c == 'O') {
        // SS3 arrows (application cursor keys)
        if (!PollReadable(40)) return KeyNone;
        int b = ReadByte();
        if (b == 'A') return KeyUp;
        if (b == 'B') return KeyDown;
        return KeyNone;
    }
    return KeyNone;
}

int ReadKey() {
    int c = ReadByte();
    if (c < 0) return -1;
    if (c == 'q' || c == 'Q') return KeyQuit;
    if (c == 3) return KeyQuit; // Ctrl-C (when not delivered as signal)
    if (c == 27) return ParseEscSequence();
    if (c == 'k' || c == 'K') return KeyUp;
    if (c == 'j' || c == 'J') return KeyDown;
    if (c == '1') return KeyTabLive;
    if (c == '2') return KeyTabDone;
    if (c == '\t') return KeyToggleTab;
    if (c == 'c' || c == 'C') return KeyClear;
    return KeyNone;
}

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

    for (const auto& t : activity.tasks()) WriteTaskRow(t, peers, color, os);
}

void RenderMonitor(const v1::PeerList& peers, const v1::ActivitySnapshot& activity, std::ostream& os, bool color,
                   int tab, int scroll, int rows, int cols) {
    const char* bold = color ? "\033[1m" : "";
    const char* dim = color ? "\033[90m" : "";
    const char* green = color ? "\033[32m" : "";
    const char* reverse = color ? "\033[7m" : "";
    const char* reset = color ? "\033[0m" : "";

    if (rows <= 0) rows = TermRows();
    if (cols <= 0) cols = TermCols();
    (void)cols;

    const v1::PeerInfo* self = nullptr;
    int others = 0;
    for (const auto& p : peers.peers()) {
        if (p.is_self()) self = &p;
        else ++others;
    }

    // FitScreen reserves the last row for the key footer.
    const int body_budget = std::max(1, rows - 1);
    const bool expand_peers = others > 0 && others <= 2 && rows >= 14;
    const int peer_lines = others == 0 ? 1 : (expand_peers ? others : 1);

    // Compact header: one self line + peers summary (keeps room for the task list).
    os << bold << "ZeroMesh" << reset;
    if (self != nullptr) {
        os << "  " << self->hostname() << "  " << self->cores_free() << "/" << self->cores_total() << " cores  "
           << Pct(self->cpu_load_pct()) << " cpu  " << Gib(self->mem_available());
    }
    os << "\n";

    if (others == 0) {
        os << "  " << dim << "(no peers yet)" << reset << "\n";
    } else if (expand_peers) {
        for (const auto& p : peers.peers()) {
            if (p.is_self()) continue;
            os << "  peer " << (p.admitted() ? green : dim) << p.hostname() << reset << "  " << p.cores_free() << "/"
               << p.cores_total() << " cores  " << (p.admitted() ? "admitted" : "waiting") << "\n";
        }
    } else {
        os << "  peers:";
        int shown = 0;
        for (const auto& p : peers.peers()) {
            if (p.is_self()) continue;
            if (shown >= 4) break;
            os << " " << (p.admitted() ? green : dim) << p.hostname() << reset << "(" << p.cores_free() << "/"
               << p.cores_total() << ")";
            ++shown;
        }
        if (others > shown) os << " " << dim << "+" << (others - shown) << reset;
        os << "\n";
    }

    int live_n = 0, done_n = 0;
    CountTasks(activity, &live_n, &done_n);
    const bool live_tab = tab != 1;

    os << "\n";
    if (live_tab) {
        os << reverse << " 1 Live " << live_n << " " << reset << "  " << dim << "2 Done " << done_n << reset << "\n";
    } else {
        os << " " << dim << "1 Live " << live_n << reset << "  " << reverse << " 2 Done " << done_n << " " << reset
           << "\n";
    }

    auto filtered = FilterTasks(activity, live_tab);
    if (filtered.empty()) {
        os << "  " << dim << (live_tab ? "(no live tasks)" : "(no finished tasks)") << reset << "\n";
        return;
    }

    os << "  " << std::left << std::setw(10) << "ROLE" << " " << std::setw(8) << "STATE" << " " << std::setw(16)
       << "WHERE" << " " << std::setw(16) << "TASK" << "  COMMAND\n";

    // title + peers + blank + tabs + colhdr already written.
    const int fixed = 1 + peer_lines + 1 + 1 + 1;
    const int total = static_cast<int>(filtered.size());
    int viewport = std::max(1, body_budget - fixed);
    if (total > viewport) viewport = std::max(1, viewport - 1); // room for "(a–b of n)"

    int start = std::clamp(scroll, 0, std::max(0, total - 1));
    if (start + viewport > total) start = std::max(0, total - viewport);
    const int end = std::min(total, start + viewport);

    for (int i = start; i < end; ++i) WriteTaskRow(*filtered[static_cast<size_t>(i)], &peers, color, os);

    if (total > viewport) {
        os << "  " << dim << "(" << (start + 1) << "–" << end << " of " << total << ")" << reset << "\n";
    }
}

int RunMonitor(const PeerFetcher& fetch_peers, const ActivityFetcher& fetch_activity,
               const ActivityClearer& clear_activity, const std::atomic<bool>& stop) {
    const bool tty = isatty(STDOUT_FILENO) != 0;
    bool read_stdin = true;
    int rc = 0;
    RawTty raw;

    int tab = 0; // 0=live, 1=done
    int scroll_live = 0;
    int scroll_done = 0;
    std::string notice;  // shown in the footer for one frame

    // Alternate screen + hide cursor; enable SGR mouse for wheel scrolling.
    if (tty) {
        std::cout << "\033[?1049h\033[?25l\033[2J\033[H\033[?1000h\033[?1006h" << std::flush;
    }

    auto cleanup = [&]() {
        if (tty) std::cout << "\033[?1006l\033[?1000l\033[?25h\033[?1049l" << std::flush;
        else std::cout << "\n" << std::flush;
    };

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

        const int rows = TermRows();
        const int cols = TermCols();
        int live_n = 0, done_n = 0;
        if (peers) CountTasks(activity, &live_n, &done_n);
        int& scroll = tab == 0 ? scroll_live : scroll_done;
        const int tab_count = tab == 0 ? live_n : done_n;
        if (scroll > 0 && scroll >= tab_count) scroll = std::max(0, tab_count - 1);

        std::ostringstream frame;
        std::string footer = "q quit · 1/2 tabs · j/k↑↓ scroll · PgUp/PgDn · c clear";
        if (peers) {
            RenderMonitor(*peers, activity, frame, tty, tab, scroll, rows, cols);
            if (tab_count > 0) {
                footer += " · ";
                footer += tab == 0 ? "Live " : "Done ";
                footer += std::to_string(tab_count);
            }
        } else {
            frame << "error: " << error << "\n";
        }
        if (!notice.empty()) {
            footer = notice + " · " + footer;
            notice.clear();
        }

        if (tty) {
            std::cout << "\033[H\033[2J" << FitScreen(frame.str(), rows, cols, footer) << std::flush;
        } else {
            std::cout << frame.str() << "\n---\n" << std::flush;
        }

        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        int ret = poll(&pfd, read_stdin ? 1 : 0, 1000);
        if (ret < 0 && errno == EINTR) continue;
        if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            // Drain all pending input so scroll keys feel responsive across refreshes.
            for (;;) {
                if (!(pfd.revents & POLLIN) && !PollReadable(0)) break;
                int key = ReadKey();
                if (key < 0) {
                    read_stdin = false;
                    break;
                }
                const int page = std::max(1, rows / 2);
                switch (key) {
                    case KeyQuit:
                        cleanup();
                        return rc;
                    case KeyUp:
                        scroll = std::max(0, scroll - 1);
                        break;
                    case KeyDown:
                        if (tab_count > 0) scroll = std::min(scroll + 1, std::max(0, tab_count - 1));
                        break;
                    case KeyPgUp:
                        scroll = std::max(0, scroll - page);
                        break;
                    case KeyPgDn:
                        if (tab_count > 0) scroll = std::min(scroll + page, std::max(0, tab_count - 1));
                        break;
                    case KeyTabLive:
                        tab = 0;
                        break;
                    case KeyTabDone:
                        tab = 1;
                        break;
                    case KeyToggleTab:
                        tab = tab == 0 ? 1 : 0;
                        break;
                    case KeyClear: {
                        std::string clear_error;
                        if (clear_activity && clear_activity(&clear_error)) {
                            scroll_live = 0;
                            scroll_done = 0;
                            notice = "cleared finished tasks";
                        } else {
                            notice = "clear failed: " + (clear_error.empty() ? "unsupported" : clear_error);
                        }
                        break;
                    }
                    default:
                        break;
                }
                pfd.revents = 0;
                if (!PollReadable(0)) break;
                pfd.revents = POLLIN;
            }
        }
    }
    cleanup();
    return rc;
}

} // namespace zeromesh::cli
