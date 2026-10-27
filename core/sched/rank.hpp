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
    int cores_total{0};  // fixed machine size (slot capacity); not live free cores
    int cores_free{0};   // informational / load hint only
    uint64_t mem_available{0};
    double cpu_load_pct{0.0};
    bool on_ac{true};
    std::string arch;
    std::chrono::milliseconds latency{0};
    int64_t missing_bytes{-1};  // input bytes the node lacks; -1 = unknown
    int in_flight{0};           // tasks this node already runs for us
    int slots{0};               // announced worker slots; 0 = size by cores_total/cores
    int queue_depth{0};         // donor queue beyond running slots (batch/fast capacity)
};

// Keeps the candidates that can run `spec` and orders them best first.
// Filter:
//   - arch equals resources.arch; an empty resources.arch means the local candidate's
//     arch (no arch filter when there is neither)
//   - free slots = slots - in_flight >= 1. A node that announces no slots gets
//     cores_total/cores (cores 0 means 1); cores_total, not live cores_free, so a busy
//     Cycles job does not shrink capacity to zero mid-job.
//   - mem_available >= mem_bytes
//   - policy.may_run_local / may_run_remote (unset = true)
// Order: free slots descending, then missing_bytes, cpu_load_pct, AC, latency, node_id.
// When candidate.missing_bytes is -1 (unknown), it is ranked as `unknown_missing_bytes`
// (typically total input transfer size — worst case).
std::vector<Candidate> Rank(const v1::TaskSpec& spec, std::vector<Candidate> candidates,
                            int64_t unknown_missing_bytes = 0);

} // namespace zeromesh::sched
