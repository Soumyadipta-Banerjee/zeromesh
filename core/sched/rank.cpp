#include "sched/rank.hpp"

#include "sched/bandwidth.hpp"

#include <algorithm>
#include <cmath>
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

// Expected-work values within this factor of each other share a bucket and are ordered by
// the later keys (missing bytes, load, ...).
constexpr double kLiveputBucketRatio = 1.05;

bool KnownLife(const Candidate& c) { return std::isfinite(c.expected_min_s) && c.expected_min_s > 0.0; }

} // namespace

double TransferCostSeconds(const Candidate& c, int64_t unknown_missing_bytes) {
    const double bandwidth =
        c.bandwidth_bytes_per_s > 0.0 ? c.bandwidth_bytes_per_s : BandwidthEstimator::kDefaultBytesPerSecond;
    return static_cast<double>(MissingForRank(c, unknown_missing_bytes)) / bandwidth;
}

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

    // Candidates without a hazard estimate count as the average known one, so missing history
    // neither helps nor hurts. If all lives are equal the liveput term is constant and the
    // order is exactly the old "most free slots" one.
    double life_min = 0.0, life_max = 0.0, life_sum = 0.0;
    int known = 0;
    for (const auto& c : candidates) {
        if (!KnownLife(c)) continue;
        life_min = known == 0 ? c.expected_min_s : std::min(life_min, c.expected_min_s);
        life_max = known == 0 ? c.expected_min_s : std::max(life_max, c.expected_min_s);
        life_sum += c.expected_min_s;
        ++known;
    }
    const double neutral_life = known == 0 ? 1.0 : (life_min == life_max ? life_min : life_sum / known);
    auto life_of = [&](const Candidate& c) { return KnownLife(c) ? c.expected_min_s : neutral_life; };
    bool liveput = false;
    for (const auto& c : candidates) {
        if (life_of(c) != life_of(candidates.front())) liveput = true;
    }
    // Higher is better. FreeSlots >= 1 and life > 0 here, so the log is defined.
    auto work = [&](const Candidate& c) -> int64_t {
        if (!liveput) return FreeSlots(c, cores);
        const double expected_work = static_cast<double>(FreeSlots(c, cores)) * life_of(c);
        return static_cast<int64_t>(std::floor(std::log(expected_work) / std::log(kLiveputBucketRatio)));
    };

    // Flamenco-style: fill the mesh. Prefer the node with the most expected committed work
    // (free slots, discounted by how long the donor is expected to stay) so work spreads
    // across peers instead of packing the requester first.
    auto key = [&](const Candidate& c) {
        return std::tuple<int64_t, double, double, bool, std::chrono::milliseconds, const std::string&>(
            -work(c), TransferCostSeconds(c, unknown_missing_bytes), c.cpu_load_pct, !c.on_ac, c.latency, c.node_id);
    };
    std::sort(candidates.begin(), candidates.end(),
              [&](const Candidate& a, const Candidate& b) { return key(a) < key(b); });
    return candidates;
}

} // namespace zeromesh::sched
