#pragma once

// Candidate ranking for placement (spec §8).

#include "zeromesh/v1/common.pb.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace zeromesh::sched {

struct Candidate {
    std::string node_id;
    bool is_local{false};
    int cores_total{0}; // fixed machine size (slot capacity); not live free cores
    int cores_free{0};  // informational / load hint only
    uint64_t mem_available{0};
    uint64_t mem_total{0};      // 0 = unknown; then mem_committed is ignored
    int64_t mem_committed{0};   // memory the tasks we already run there declared (resources.mem_bytes)
    int core_limit{-1};         // cores the donor says it donates in total (its donor policy); -1 = unknown
    double cpu_load_pct{0.0};
    bool on_ac{true};
    std::string arch;
    std::chrono::milliseconds latency{0};
    int64_t missing_bytes{-1}; // input bytes the node lacks; -1 = unknown
    int in_flight{0};          // tasks this node already runs for us
    // E[min(remaining life, task time)] in seconds from the departure-hazard model (sched/hazard.hpp);
    // <= 0 or non-finite = unknown (no history).
    double expected_min_s{-1.0};
    double bandwidth_bytes_per_s{0.0}; // measured throughput to the node; 0 = unmeasured (default assumed)
};

// Assumed task length when the caller has no estimate.
inline constexpr double kDefaultTaskSeconds = 60.0;

// Estimated seconds to move the candidate's missing input to it: missing bytes (the caller's
// `unknown_missing_bytes` when unknown) over its measured bandwidth (BandwidthEstimator's
// default when unmeasured).
double TransferCostSeconds(const Candidate& c, int64_t unknown_missing_bytes);

// Keeps the candidates that can run `spec` and orders them best first.
// Filter:
//   - arch equals resources.arch; an empty resources.arch means the local candidate's
//     arch (no arch filter when there is neither)
//   - slots = min(cores_total, core_limit)/cores - in_flight >= 1 (cores 0 means 1). Uses
//     cores_total so a busy Cycles job does not shrink capacity to zero mid-job; core_limit
//     (what the donor's policy lets it donate, once it has told us) caps it, since a donor
//     refuses what its limit does not cover however big the machine is.
//   - mem_available >= mem_bytes, and mem_total - mem_committed >= mem_bytes when mem_total is
//     known: the tasks we already placed there have not necessarily touched their memory yet,
//     so mem_available alone lets several of them pile onto one donor
//   - policy.may_run_local / may_run_remote (unset = true)
// Order: expected committed work descending, then transfer time (missing_bytes / measured bandwidth), cpu_load_pct, AC, latency,
// node_id. Expected committed work is free slots x expected_min_s x task_s / (task_s +
// transfer time): Parcae-style liveput (a slot on a donor that will probably vanish mid-task
// is worth less), and a slot that first has to wait for its input is worth less too, by the
// share of the task's time the transfer takes. A donor missing 5 GB of a 60 s task therefore
// loses to one that holds the input even if it has a few more slots. A candidate with unknown
// expected_min_s counts as the average of the known ones. When every candidate has the same
// life (including none known: no history) the work is just the free-slot count, i.e. the
// plain "most free slots first" order. When lives differ, work is compared in ~5% buckets,
// so near-equal work still falls through to the missing_bytes tie-break.
// When candidate.missing_bytes is -1 (unknown), it is ranked as `unknown_missing_bytes`
// (typically total input transfer size — worst case).
// `task_s` is how long the task is expected to run, the yardstick for what a transfer costs.
std::vector<Candidate> Rank(const v1::TaskSpec& spec, std::vector<Candidate> candidates,
                            int64_t unknown_missing_bytes = 0, double task_s = kDefaultTaskSeconds);

} // namespace zeromesh::sched
