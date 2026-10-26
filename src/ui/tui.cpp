#include "ui/tui.hpp"
#include "executor/host_guard.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace zeromesh::ui {

TerminalUI::TerminalUI(
    std::shared_ptr<discovery::PeerTable> peer_table,
    std::shared_ptr<scheduler::Scheduler> scheduler
) : peer_table_(std::move(peer_table)), scheduler_(std::move(scheduler)) {}

static std::string RenderProgressBar(double pct, int width = 20) {
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;

    int filled = static_cast<int>((pct / 100.0) * width);
    std::string bar = "[";
    for (int i = 0; i < filled; ++i) bar += "█";
    for (int i = filled; i < width; ++i) bar += "░";
    bar += "] ";

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << pct << "%";
    bar += ss.str();
    return bar;
}

void TerminalUI::PrintStatusSnapshot() const {
    std::cout << "\033[1;36m"
              << "================================================================================\n"
              << "             ZeroMesh Remote Execution Mesh - Live Cluster Topology             \n"
              << "================================================================================\033[0m\n";

    // 1. Host Machine Telemetry
    double local_load = executor::HostGuard::GetLocalCpuLoadPct();
    int battery = executor::HostGuard::GetBatteryPercent();
    bool ac = executor::HostGuard::IsAcConnected();

    std::cout << "\033[1;33m[ Local Host Status ]\033[0m\n"
              << "  CPU Load:  " << RenderProgressBar(local_load, 24) << "\n"
              << "  Power:     " << (ac ? "\033[32m⚡ AC Connected\033[0m" : "\033[33m🔋 On Battery\033[0m")
              << " (" << battery << "%)\n"
              << "  Protocol:  Google Remote Execution API (REAPI v2 over gRPC)\n"
              << "  Endpoint:  127.0.0.1:8980\n\n";

    // 2. Scheduler Telemetry
    if (scheduler_) {
        std::cout << "\033[1;33m[ DAG Scheduler & Cache Metrics ]\033[0m\n"
                  << "  Total Actions:  " << scheduler_->GetTotalScheduled() << "\n"
                  << "  Pinned Local:   " << scheduler_->GetTotalLocal() << " (Linkers, archivers, & hot critical paths)\n"
                  << "  Offloaded:      " << scheduler_->GetTotalOffloaded() << " (Distributed parallel compile tasks)\n\n";
    }

    // 3. Cluster Peer Nodes Table
    std::cout << "\033[1;33m[ Discovered Mesh Workers (mDNS / DHT) ]\033[0m\n";
    auto active_peers = peer_table_ ? peer_table_->ListActive() : std::vector<discovery::Peer>{};

    if (active_peers.empty()) {
        std::cout << "  \033[90m(No remote peer nodes discovered yet. Listening on LAN UDP multicast 239.255.42.99:8982...)\033[0m\n\n";
    } else {
        std::cout << "  ┌──────────────────────┬──────────────────────┬─────────┬─────────┬──────────────┬──────────┐\n"
                  << "  │ Node ID              │ Hostname & Endpoint  │ Cores   │ RAM     │ CPU Load     │ Status   │\n"
                  << "  ├──────────────────────┼──────────────────────┼─────────┼─────────┼──────────────┼──────────┤\n";

        for (const auto& peer : active_peers) {
            std::string id_sub = peer.id.size() > 20 ? peer.id.substr(0, 17) + "..." : peer.id;
            std::string host_ep = peer.hostname + " (" + peer.endpoint() + ")";
            if (host_ep.size() > 20) host_ep = host_ep.substr(0, 17) + "...";

            double ram_gb = static_cast<double>(peer.memory_bytes) / (1024.0 * 1024.0 * 1024.0);
            std::ostringstream ram_ss;
            ram_ss << std::fixed << std::setprecision(1) << ram_gb << " GB";

            std::ostringstream load_ss;
            load_ss << std::fixed << std::setprecision(1) << peer.cpu_load_pct << "%";

            std::string status_str = (peer.state == discovery::PeerState::Active) ? "\033[32mActive\033[0m" : "\033[31mDraining\033[0m";

            std::cout << "  │ " << std::left << std::setw(20) << id_sub
                      << " │ " << std::left << std::setw(20) << host_ep
                      << " │ " << std::right << std::setw(7) << peer.cores
                      << " │ " << std::right << std::setw(7) << ram_ss.str()
                      << " │ " << std::right << std::setw(12) << load_ss.str()
                      << " │ " << std::left << std::setw(8) << status_str << " │\n";
        }

        std::cout << "  └──────────────────────┴──────────────────────┴─────────┴─────────┴──────────────┴──────────┘\n\n";
    }

    std::cout << "\033[1;36m================================================================================\033[0m\n";
}

void TerminalUI::RunLiveDashboard() {
    keep_running_ = true;

    // Hide terminal cursor
    std::cout << "\033[?25l\033[2J";

    struct pollfd stdin_poll;
    stdin_poll.fd = STDIN_FILENO;
    stdin_poll.events = POLLIN;

    while (keep_running_.load()) {
        // Move to top-left
        std::cout << "\033[H";
        PrintStatusSnapshot();
        std::cout << "\n\033[90mPress 'q' or Ctrl+C to exit interactive monitor...\033[0m" << std::flush;

        // Poll for interactive user input for 500ms
        int ret = poll(&stdin_poll, 1, 500);
        if (ret > 0 && (stdin_poll.revents & POLLIN)) {
            char ch = 0;
            if (read(STDIN_FILENO, &ch, 1) > 0) {
                if (ch == 'q' || ch == 'Q' || ch == 27) { // 'q' or ESC
                    keep_running_ = false;
                    break;
                }
            }
        }
    }

    // Restore terminal cursor
    std::cout << "\033[?25h\n";
}

} // namespace zeromesh::ui
