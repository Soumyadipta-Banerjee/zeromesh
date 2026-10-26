#include "ui/tui.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace zeromesh::ui {

TerminalUI::TerminalUI(std::shared_ptr<net::PeerTable> peer_table)
    : peer_table_(std::move(peer_table)) {}

void TerminalUI::PrintStatusSnapshot() const {
    std::cout << "\033[1;36m"
              << "================================================================================\n"
              << "             ZeroMesh Remote Execution Mesh - Live Cluster Topology             \n"
              << "================================================================================\033[0m\n";

    // 1. Host Machine Telemetry


    // 3. Cluster Peer Nodes Table
    std::cout << "\033[1;33m[ Discovered Mesh Workers (LAN multicast) ]\033[0m\n";
    auto active_peers = peer_table_ ? peer_table_->ListActive() : std::vector<net::Peer>{};

    if (active_peers.empty()) {
        std::cout << "  \033[90m(No remote peer nodes discovered yet. Listening on LAN UDP multicast 239.255.42.99:8988...)\033[0m\n\n";
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

            std::string status_str = (peer.state == net::PeerState::Active) ? "\033[32mActive\033[0m" : "\033[31mDraining\033[0m";

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
