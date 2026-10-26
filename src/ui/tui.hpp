#pragma once

#include "discovery/peer_table.hpp"
#include "scheduler/scheduler.hpp"

#include <atomic>
#include <memory>

namespace zeromesh::ui {

/**
 * @brief Terminal dashboard visualizer showing live cluster topology,
 * per-peer CPU utilization, network transfer speeds, and compilation throughput.
 */
class TerminalUI {
public:
    TerminalUI(
        std::shared_ptr<discovery::PeerTable> peer_table,
        std::shared_ptr<scheduler::Scheduler> scheduler
    );
    ~TerminalUI() = default;

    /**
     * @brief Launches full-screen interactive or live-updating terminal TUI.
     */
    void RunLiveDashboard();

    /**
     * @brief Prints a single static snapshot status summary to stdout.
     */
    void PrintStatusSnapshot() const;

private:
    std::shared_ptr<discovery::PeerTable> peer_table_;
    std::shared_ptr<scheduler::Scheduler> scheduler_;
    std::atomic<bool> keep_running_{true};
};

} // namespace zeromesh::ui
