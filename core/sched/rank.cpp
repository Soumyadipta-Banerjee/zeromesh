#include "sched/rank.hpp"

#include <algorithm>
#include <tuple>

namespace zeromesh::sched {

namespace {

// How many more tasks of `cores` this node can take right now.
int FreeSlots(const Candidate& c, int cores) {
    const int per = std::max(cores, 1);
    const int capacity = std::max(c.cores_free, 0) / per;
    return capacity - c.in_flight;
}

} // namespace

std::vector<Candidate> Rank(const v1::TaskSpec& spec, std::vector<Candidate> candidates) {
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
        const bool unknown = c.missing_bytes < 0;
        return std::tuple<int, bool, int64_t, double, bool, std::chrono::milliseconds, const std::string&>(
            -FreeSlots(c, cores), unknown, unknown ? 0 : c.missing_bytes, c.cpu_load_pct, !c.on_ac, c.latency,
            c.node_id);
    };
    std::sort(candidates.begin(), candidates.end(),
              [&](const Candidate& a, const Candidate& b) { return key(a) < key(b); });
    return candidates;
}

} // namespace zeromesh::sched
