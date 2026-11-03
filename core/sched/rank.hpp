#pragma once

// Candidate ranking for placement (spec §8).

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace zeromesh::sched {

// What a batch needs of the node it runs on.
struct Need {
    int cores{1};
    int64_t mem_bytes{0}; // reserved, to pack it beside other batches
    int64_t mem_need{0};  // known to need at least: a node that lets work use less cannot run it
    int64_t mem_limit{0}; // a hard limit it may use up to: likewise
    bool alone{false};    // must have the node's memory for work to itself
    int gpus{0};
    std::string arch; // "" = the local candidate's
    bool may_local{true};
    bool may_remote{true};
};

struct Candidate {
    std::string node_id;
    bool is_local{false};
    // Cores work may hold there: what a peer announces (work_cores), this node's executor slots.
    // Not what is idle this second: a busy render must not make the machine look gone.
    int work_cores{0};
    int cores_in_use{0}; // of those, held now: by our batches on a peer, by every batch in this node's executor
    uint64_t mem_available{0};
    uint64_t mem_total{0};      // 0 = unknown; then mem_committed is ignored
    int64_t mem_committed{0};   // memory held there, as cores_in_use (the batches' reservations)
    double cpu_load_pct{0.0};
    bool on_ac{true};
    std::string arch;
    std::chrono::milliseconds latency{0};
    int64_t missing_bytes{-1}; // input bytes the node lacks; -1 = unknown
    // E[min(remaining life, task time)] in seconds from the departure-hazard model (sched/hazard.hpp);
    // <= 0 or non-finite = unknown (no history).
    double expected_min_s{-1.0};
    double bandwidth_bytes_per_s{0.0}; // measured throughput to the node; 0 = unmeasured (default assumed)
    int gpus{0};             // GPUs the node has
    int gpus_in_flight{0};   // GPUs our batches there hold
};

// What a batch with `need` holds of the candidate's memory for work (mem_total): its reservation,
// at most all of it, and all of it when it must run alone. Unknown mem_total: the reservation.
int64_t HeldOn(const Candidate& c, const Need& need);

// Assumed task length when the caller has no estimate.
inline constexpr double kDefaultTaskSeconds = 60.0;

// Estimated seconds to move the candidate's missing input to it: missing bytes (the caller's
// `unknown_missing_bytes` when unknown) over its measured bandwidth (BandwidthEstimator's
// default when unmeasured).
double TransferCostSeconds(const Candidate& c, int64_t unknown_missing_bytes);

// Keeps the candidates that can run a batch with `need` and orders them best first.
// Filter:
//   - arch equals need.arch; an empty one means the local candidate's
//     arch (no arch filter when there is neither)
//   - free slots = (work_cores - cores_in_use) / cores >= 1 (cores 0 means 1)
//   - mem_need and mem_limit at most mem_total (the memory the node lets work use), and what the
//     batch holds there (HeldOn) at most mem_available and mem_total - mem_committed: the batches
//     we already placed there have not necessarily touched their memory yet
//   - need.gpus free GPUs
//   - need.may_local / may_remote
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
std::vector<Candidate> Rank(const Need& need, std::vector<Candidate> candidates,
                            int64_t unknown_missing_bytes = 0, double task_s = kDefaultTaskSeconds);

} // namespace zeromesh::sched
