#include "sched/rank.hpp"

#include <algorithm>
#include <tuple>

namespace zeromesh::sched {

namespace {

// How many more tasks of `cores` this node can take. Capacity is cores_total
// (fixed), not live cores_free — otherwise a full-CPU Blender run makes Rank
// think the machine has vanished and waiting tasks hard-fail.
int FreeSlots(const Candidate& c, int cores) {
    const int per = std::max(cores, 1);
    int total = c.cores_total > 0 ? c.cores_total : c.cores_free;
    const int capacity = std::max(total, 0) / per;
    return capacity - c.in_flight;
}

int64_t MissingForRank(const Candidate& c, int64_t unknown_missing_bytes) {
    if (c.missing_bytes >= 0) return c.missing_bytes;
    return std::max<int64_t>(0, unknown_missing_bytes);
}

} // namespace

std::vector<Candidate> Rank(const v1::TaskSpec& spec, std::vector<Candidate> candidates,
                            int64_t unknown_missing_bytes) {
    const auto& policy = spec.policy();
    const bool may_local = !policy.has_may_run_local() || policy.may_run_local();
    const bool may_remote = !policy.has_may_run_remote() || policy.may_run_remote();
    const int cores = std::max(spec.resources().cores(), 1);
    const int64_t mem = spec.resources().mem_bytes();

    std::string arch = spec.resources().arch();
    if (arch.empty()) {
        for (const auto& c : candidates) {
            if (c.is_local) arch = c.arch;
        }
    }

    std::erase_if(candidates, [&](const Candidate& c) {
        if (c.is_local ? !may_local : !may_remote) return true;
        if (!arch.empty() && c.arch != arch) return true;
        if (FreeSlots(c, cores) < 1) return true;
        if (mem > 0 && c.mem_available < static_cast<uint64_t>(mem)) return true;
        return false;
    });

    // Flamenco-style: fill the mesh. Prefer the node with the most free slots so
    // work spreads across peers instead of packing the requester first.
    auto key = [&](const Candidate& c) {
        return std::tuple<int, int64_t, double, bool, std::chrono::milliseconds, const std::string&>(
            -FreeSlots(c, cores), MissingForRank(c, unknown_missing_bytes), c.cpu_load_pct, !c.on_ac, c.latency,
            c.node_id);
    };
    std::sort(candidates.begin(), candidates.end(),
              [&](const Candidate& a, const Candidate& b) { return key(a) < key(b); });
    return candidates;
}

} // namespace zeromesh::sched
