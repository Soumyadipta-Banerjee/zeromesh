#include "scheduler/scheduler.hpp"

namespace zeromesh::scheduler {

Scheduler::Scheduler(std::shared_ptr<discovery::PeerTable> peer_table)
    : peer_table_(std::move(peer_table)) {}

ScheduleDecision Scheduler::Decide(const ActionTask& task, double local_cpu_load_pct) {
    total_scheduled_++;

    // 1. Critical path check: linkers, final binaries, and fat archives must stay local
    if (SchedulerHeuristics::IsCriticalPath(task.command_args)) {
        total_local_++;
        return ScheduleDecision{
            .target = DispatchTarget::Local,
            .remote_peer = std::nullopt,
            .reason = "Critical path action pinned locally to prevent network serialization"
        };
    }

    // 2. Discover available remote donor nodes
    auto best_donor = peer_table_ ? peer_table_->SelectBestWorker() : std::nullopt;
    if (!best_donor.has_value()) {
        total_local_++;
        return ScheduleDecision{
            .target = DispatchTarget::Local,
            .remote_peer = std::nullopt,
            .reason = "No active peer nodes found on local network"
        };
    }

    // 3. Evaluate cost heuristics (network latency + missing bytes vs compute saturation)
    double donor_idle_pct = 100.0 - best_donor->cpu_load_pct;
    bool should_offload = SchedulerHeuristics::ShouldOffload(
        task.missing_bytes_on_peer,
        donor_idle_pct,
        local_cpu_load_pct,
        best_donor->latency
    );

    if (should_offload) {
        total_offloaded_++;
        return ScheduleDecision{
            .target = DispatchTarget::Remote,
            .remote_peer = best_donor,
            .reason = "Offloaded to donor: " + best_donor->hostname + " (" + best_donor->endpoint() + ")"
        };
    }

    total_local_++;
    return ScheduleDecision{
        .target = DispatchTarget::Local,
        .remote_peer = std::nullopt,
        .reason = "Local execution preferred due to input data size or low local load"
    };
}

} // namespace zeromesh::scheduler
