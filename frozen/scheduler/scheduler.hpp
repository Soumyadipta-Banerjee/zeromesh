#pragma once

#include "discovery/peer_table.hpp"
#include "scheduler/heuristics.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zeromesh::scheduler {

struct ActionTask {
    std::string action_digest_hash;
    std::vector<std::string> command_args;
    std::vector<std::pair<std::string, std::string>> env_vars;
    std::string input_root_digest_hash;
    int64_t total_input_bytes{0};
    int64_t missing_bytes_on_peer{0};
    std::string working_directory{"."};
};

enum class DispatchTarget {
    Local,
    Remote
};

struct ScheduleDecision {
    DispatchTarget target{DispatchTarget::Local};
    std::optional<discovery::Peer> remote_peer;
    std::string reason;
};

/**
 * @brief Locality-aware and DAG-critical-path task scheduler.
 * Balances cluster workloads across nearby laptops while keeping serial critical paths local.
 */
class Scheduler {
public:
    explicit Scheduler(std::shared_ptr<discovery::PeerTable> peer_table);
    ~Scheduler() = default;

    /**
     * @brief Makes an optimal scheduling decision for an incoming REAPI build action.
     */
    [[nodiscard]] ScheduleDecision Decide(const ActionTask& task, double local_cpu_load_pct);

    /**
     * @brief Scheduler counters.
     */
    [[nodiscard]] uint64_t GetTotalScheduled() const { return total_scheduled_.load(); }
    [[nodiscard]] uint64_t GetTotalOffloaded() const { return total_offloaded_.load(); }
    [[nodiscard]] uint64_t GetTotalLocal() const { return total_local_.load(); }

private:
    std::shared_ptr<discovery::PeerTable> peer_table_;
    std::atomic<uint64_t> total_scheduled_{0};
    std::atomic<uint64_t> total_offloaded_{0};
    std::atomic<uint64_t> total_local_{0};
};

} // namespace zeromesh::scheduler
