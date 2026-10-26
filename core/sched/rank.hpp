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
    int cores_free{0};
    uint64_t mem_available{0};
    double cpu_load_pct{0.0};
    bool on_ac{true};
    std::string arch;
    std::chrono::milliseconds latency{0};
    int64_t missing_bytes{-1};  // input bytes the node lacks; -1 = unknown
    int in_flight{0};           // tasks this node already runs for us
};

// Keeps the candidates that can run `spec` and orders them best first.
// Filter:
//   - arch equals resources.arch; an empty resources.arch means the local candidate's
//     arch (no arch filter when there is neither)
//   - cores_free - in_flight * cores >= cores (cores 0 means 1)
//   - mem_available >= mem_bytes
//   - policy.may_run_local / may_run_remote (unset = true)
// Order: free slots descending (cores_free/cores - in_flight) so work spreads across the
// mesh like a render farm queue, then missing_bytes ascending (unknown after known),
// cpu_load_pct ascending, on AC first, latency ascending, then node_id.
std::vector<Candidate> Rank(const v1::TaskSpec& spec, std::vector<Candidate> candidates);

} // namespace zeromesh::sched
