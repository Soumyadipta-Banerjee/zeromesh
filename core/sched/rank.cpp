#include "sched/rank.hpp"

#include "sched/bandwidth.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace zeromesh::sched {

namespace {

// How many more batches of `cores` this node can take.
int FreeSlots(const Candidate& c, int cores) {
    return std::max(c.work_cores - c.cores_in_use, 0) / std::max(cores, 1);
}

int64_t MissingForRank(const Candidate& c, int64_t unknown_missing_bytes) {
    if (c.missing_bytes >= 0) return c.missing_bytes;
    return std::max<int64_t>(0, unknown_missing_bytes);
}

// Expected-work values within this factor of each other share a bucket and are ordered by
// the later keys (missing bytes, load, ...).
constexpr double kLiveputBucketRatio = 1.05;

// What the batch needs must fit the node's memory for work at all; what it holds there must fit in
// what is available and beside the batches we already placed there.
bool FitsMemory(const Candidate& c, const Need& need) {
    const auto total = static_cast<int64_t>(c.mem_total);
    if (total > 0 && (need.mem_need > total || need.mem_limit > total)) return false;
    const int64_t held = HeldOn(c, need);
    if (held <= 0) return true;
    if (c.mem_available < static_cast<uint64_t>(held)) return false;
    return total <= 0 || total - std::max<int64_t>(c.mem_committed, 0) >= held;
}

// A transfer under this share of the task's time is noise; ordering then stays on slots alone.
constexpr double kTransferNoiseShare = 0.01;

bool KnownLife(const Candidate& c) { return std::isfinite(c.expected_min_s) && c.expected_min_s > 0.0; }

} // namespace

int64_t HeldOn(const Candidate& c, const Need& need) {
    const int64_t reserved = std::max<int64_t>(need.mem_bytes, 0);
    const auto total = static_cast<int64_t>(c.mem_total);
    if (total <= 0) return reserved;
    return need.alone ? total : std::min(reserved, total);
}

double TransferCostSeconds(const Candidate& c, int64_t unknown_missing_bytes) {
    const double bandwidth =
        c.bandwidth_bytes_per_s > 0.0 ? c.bandwidth_bytes_per_s : BandwidthEstimator::kDefaultBytesPerSecond;
    return static_cast<double>(MissingForRank(c, unknown_missing_bytes)) / bandwidth;
}

std::vector<Candidate> Rank(const Need& need, std::vector<Candidate> candidates, int64_t unknown_missing_bytes,
                            double task_s) {
    if (!(task_s > 0.0) || !std::isfinite(task_s)) task_s = kDefaultTaskSeconds;
    const bool may_local = need.may_local;
    const bool may_remote = need.may_remote;
    const int cores = std::max(need.cores, 1);

    std::string arch = need.arch;
    if (arch.empty()) {
        for (const auto& c : candidates) {
            if (c.is_local) arch = c.arch;
        }
    }

    std::erase_if(candidates, [&](const Candidate& c) {
        if (c.is_local ? !may_local : !may_remote) return true;
        if (!arch.empty() && c.arch != arch) return true;
        if (FreeSlots(c, cores) < 1) return true;
        if (!FitsMemory(c, need)) return true;
        if (need.gpus > 0 && c.gpus - c.gpus_in_flight < need.gpus) return true;
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
    // Share of the task's time that is useful once the input has arrived.
    auto efficiency = [&](const Candidate& c) {
        return task_s / (task_s + TransferCostSeconds(c, unknown_missing_bytes));
    };
    bool transfer_matters = false;
    for (const auto& c : candidates) {
        if (TransferCostSeconds(c, unknown_missing_bytes) > kTransferNoiseShare * task_s) transfer_matters = true;
    }
    // Higher is better. FreeSlots >= 1, life > 0 and efficiency > 0 here, so the log is defined.
    auto work = [&](const Candidate& c) -> int64_t {
        if (!liveput && !transfer_matters) return FreeSlots(c, cores);
        const double expected_work = static_cast<double>(FreeSlots(c, cores)) * life_of(c) * efficiency(c);
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
