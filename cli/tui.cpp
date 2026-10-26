#include "cli/tui.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <iomanip>
#include <iostream>
#include <sstream>

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
        // First 8 chars of the node id, per the plan's status layout.
        std::string id8 = p.node_id().size() <= 8 ? p.node_id() : p.node_id().substr(0, 8);
        os << "  " << std::left << std::setw(10) << id8 << "  " << std::setw(24) << Clip(host, 24)
           << "  " << std::right << std::setw(9) << cores << "  " << std::setw(9) << Gib(p.mem_available()) << "  "
           << std::setw(7) << Pct(p.cpu_load_pct()) << "  " << state_color << std::left << std::setw(10)
           << p.state() << reset << "  " << (p.admitted() ? "yes" : "no") << "\n";
    }
}

int RunMonitor(const PeerFetcher& fetch, const std::atomic<bool>& stop) {
    const bool tty = isatty(STDOUT_FILENO) != 0;
    bool read_stdin = true;
    int rc = 0;
    std::cout << "\033[?25l\033[2J" << std::flush;  // hide the cursor, clear the screen

    while (!stop.load()) {
        std::string error;
        bool unavailable = false;
        auto peers = fetch(&error, &unavailable);
        if (!peers && unavailable) {
            rc = 2;
            break;
        }
        std::ostringstream frame;
        if (peers) RenderStatus(*peers, frame, tty);
        else frame << "error: " << error << "\n";
        frame << "\n" << (tty ? "\033[90m" : "") << "Press q or Ctrl-C to exit" << (tty ? "\033[0m" : "");
        std::cout << "\033[H\033[J" << frame.str() << std::flush;

        pollfd pfd{STDIN_FILENO, POLLIN, 0};
        int ret = poll(&pfd, read_stdin ? 1 : 0, 1000);
        if (ret < 0 && errno == EINTR) continue;
        if (ret > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char ch = 0;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0) read_stdin = false;  // EOF: stop watching stdin, keep refreshing
            else if (ch == 'q' || ch == 'Q' || ch == 27) break;
        }
    }
    std::cout << "\033[?25h\n" << std::flush;  // restore the cursor
    return rc;
}

} // namespace zeromesh::cli
