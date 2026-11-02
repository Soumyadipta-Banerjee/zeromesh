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

std::string Clip(const std::string& s, size_t n) {
    return s.size() > n ? s.substr(0, n - 3) + "..." : s;
}

// A run's progress as a bar of `width` cells.
std::string Bar(const v1::RunStatus& r, int width, bool color) {
    const int total = std::max(r.total(), 1);
    const int done = r.succeeded() + r.failed() + r.cancelled();
    const int filled = static_cast<int>(static_cast<int64_t>(width) * done / total);
    std::string out = color ? "\033[32m" : "";
    for (int i = 0; i < width; ++i) out += i < filled ? "#" : (color ? "\033[90m." : ".");
    if (color) out += "\033[0m";
    return out;
}

std::string RunState(const v1::RunStatus& r) {
    if (!r.done()) return r.running() > 0 ? "running" : "queued";
    if (r.ended_by() == "cancel") return "cancelled";
    if (!r.ended_by().empty() && r.ended_by() != "failure") return r.ended_by();
    return r.failed() > 0 ? "failed" : "done";
}

void WriteRunRow(const v1::RunStatus& r, bool color, std::ostream& os) {
    const char* red = color ? "\033[31m" : "";
    const char* cyan = color ? "\033[36m" : "";
    const char* dim = color ? "\033[90m" : "";
    const char* reset = color ? "\033[0m" : "";
    const int done = r.succeeded() + r.failed() + r.cancelled();
    std::string name = r.name().empty() ? "run" : r.name();
    if (name.size() > 14) name = name.substr(0, 11) + "...";
    os << "  " << std::left << std::setw(12) << r.run_id() << " " << std::setw(14) << name << " " << Bar(r, 20, color)
       << " " << std::right << std::setw(6) << done << "/" << std::left << std::setw(6) << r.total();
    if (r.running() > 0) os << " " << r.running() << " running";
    if (r.cached() > 0) os << " " << cyan << r.cached() << " cached" << reset;
    if (r.failed() > 0) os << " " << red << r.failed() << " failed" << reset;
    os << " " << dim << RunState(r) << reset << "\n";
}

void WriteBatchRow(const v1::ActiveBatch& b, const v1::PeerList* peers, bool color, std::ostream& os) {
    const char* green = color ? "\033[32m" : "";
    const char* dim = color ? "\033[90m" : "";
    const char* reset = color ? "\033[0m" : "";
    std::string from = b.requester().size() <= 8 ? b.requester() : b.requester().substr(0, 8);
    if (peers) {
        for (const auto& p : peers->peers()) {
            if (p.node_id() == b.requester()) from = p.is_self() ? "here" : p.hostname();
        }
    }
    os << "  " << (b.running() ? green : dim) << std::left << std::setw(8) << (b.running() ? "running" : "queued")
       << reset << " " << std::setw(16) << Clip(from, 16) << " " << std::setw(12) << Clip(b.kind().empty() ? "-" : b.kind(), 12)
       << " " << std::right << std::setw(5) << b.done() << "/" << std::left << std::setw(5) << b.items() << " "
       << Clip(b.current(), 32) << "\n";
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

void CountRows(const v1::ActivitySnapshot& activity, int* live, int* done) {
    *live = activity.batches_size();
    *done = 0;
    for (const auto& r : activity.runs()) {
        if (r.done())
            ++(*done);
        else
            ++(*live);
    }
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
    KeyTabLive = 6, // switch to live (1)
    KeyTabDone = 7, // switch to done (2)
    KeyToggleTab = 8,
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
    os << "  " << std::left << std::setw(10) << "NODE ID" << "  " << std::setw(24) << "HOST" << "  " << std::right
       << std::setw(9) << "CORES" << "  " << std::setw(9) << "MEM" << "  " << std::setw(7) << "CPU"
       << "  " << std::left << std::setw(10) << "STATE" << "  ADMITTED\n";
    for (const auto& p : peers.peers()) {
        if (p.is_self()) continue;
        std::string host = p.hostname() + " (" + p.address() + ":" + std::to_string(p.node_port()) + ")";
        std::string cores = std::to_string(p.cores_free()) + "/" + std::to_string(p.cores_total());
        const char* state_color = p.state() == "active" ? green : yellow;
        std::string id8 = p.node_id().size() <= 8 ? p.node_id() : p.node_id().substr(0, 8);
        os << "  " << std::left << std::setw(10) << id8 << "  " << std::setw(24) << Clip(host, 24) << "  " << std::right
           << std::setw(9) << cores << "  " << std::setw(9) << Gib(p.mem_available()) << "  " << std::setw(7)
           << Pct(p.cpu_load_pct()) << "  " << state_color << std::left << std::setw(10) << p.state() << reset << "  "
           << (p.admitted() ? "yes" : "no") << "\n";
    }
}

void RenderActivity(const v1::ActivitySnapshot& activity, std::ostream& os, bool color, const v1::PeerList* peers) {
    const char* bold = color ? "\033[1m" : "";
    const char* dim = color ? "\033[90m" : "";
    const char* reset = color ? "\033[0m" : "";
    os << bold << "Runs" << reset << "\n";
    if (activity.runs_size() == 0) os << "  " << dim << "(no runs yet)" << reset << "\n";
    for (const auto& r : activity.runs()) WriteRunRow(r, color, os);
    if (activity.batches_size() > 0) {
        os << bold << "Working here" << reset << "\n";
        for (const auto& b : activity.batches()) WriteBatchRow(b, peers, color, os);
    }
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
        if (p.is_self())
            self = &p;
        else
            ++others;
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
    CountRows(activity, &live_n, &done_n);
    const bool live_tab = tab != 1;

    os << "\n";
    if (live_tab) {
        os << reverse << " 1 Live " << live_n << " " << reset << "  " << dim << "2 Done " << done_n << reset << "\n";
    } else {
        os << " " << dim << "1 Live " << live_n << reset << "  " << reverse << " 2 Done " << done_n << " " << reset
           << "\n";
    }

    // One line per row: running runs and the batches this node runs for others, or finished runs.
    std::vector<std::string> lines;
    for (const auto& r : activity.runs()) {
        if (r.done() == live_tab) continue;
        std::ostringstream line;
        WriteRunRow(r, color, line);
        lines.push_back(line.str());
    }
    if (live_tab) {
        for (const auto& b : activity.batches()) {
            std::ostringstream line;
            WriteBatchRow(b, &peers, color, line);
            lines.push_back(line.str());
        }
    }
    if (lines.empty()) {
        os << "  " << dim << (live_tab ? "(nothing running)" : "(no finished runs)") << reset << "\n";
        return;
    }
    const int fixed = 1 + peer_lines + 1 + 1;
    const int total = static_cast<int>(lines.size());
    int viewport = std::max(1, body_budget - fixed);
    if (total > viewport) viewport = std::max(1, viewport - 1);
    int start = std::clamp(scroll, 0, std::max(0, total - 1));
    if (start + viewport > total) start = std::max(0, total - viewport);
    const int end = std::min(total, start + viewport);
    for (int i = start; i < end; ++i) os << lines[static_cast<size_t>(i)];
    if (total > viewport) {
        os << "  " << dim << "(" << (start + 1) << "–" << end << " of " << total << ")" << reset << "\n";
    }
}

int RunMonitor(const PeerFetcher& fetch_peers, const ActivityFetcher& fetch_activity, const std::atomic<bool>& stop) {
    const bool tty = isatty(STDOUT_FILENO) != 0;
    bool read_stdin = true;
    int rc = 0;
    RawTty raw;

    int tab = 0; // 0=live, 1=done
    int scroll_live = 0;
    int scroll_done = 0;

    // Alternate screen + hide cursor; enable SGR mouse for wheel scrolling.
    if (tty) {
        std::cout << "\033[?1049h\033[?25l\033[2J\033[H\033[?1000h\033[?1006h" << std::flush;
    }

    auto cleanup = [&]() {
        if (tty)
            std::cout << "\033[?1006l\033[?1000l\033[?25h\033[?1049l" << std::flush;
        else
            std::cout << "\n" << std::flush;
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
        if (peers) CountRows(activity, &live_n, &done_n);
        int& scroll = tab == 0 ? scroll_live : scroll_done;
        const int tab_count = tab == 0 ? live_n : done_n;
        if (scroll > 0 && scroll >= tab_count) scroll = std::max(0, tab_count - 1);

        std::ostringstream frame;
        std::string footer = "q quit · 1/2 tabs · j/k↑↓ scroll · PgUp/PgDn";
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
