#include "sched/bandwidth.hpp"

namespace zeromesh::sched {

void BandwidthEstimator::Observe(const std::string& node_id, uint64_t bytes, std::chrono::nanoseconds elapsed) {
    if (bytes < kMinSampleBytes || elapsed.count() <= 0) return;
    const double seconds = static_cast<double>(elapsed.count()) / 1e9;
    const double sample = static_cast<double>(bytes) / seconds;
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, inserted] = bytes_per_second_.try_emplace(node_id, sample);
    if (!inserted) it->second = kAlpha * sample + (1.0 - kAlpha) * it->second;
}

double BandwidthEstimator::Estimate(const std::string& node_id) const {
    return Measured(node_id).value_or(kDefaultBytesPerSecond);
}

std::optional<double> BandwidthEstimator::Measured(const std::string& node_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = bytes_per_second_.find(node_id);
    if (it == bytes_per_second_.end()) return std::nullopt;
    return it->second;
}

} // namespace zeromesh::sched
