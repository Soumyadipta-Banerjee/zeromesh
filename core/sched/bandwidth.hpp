#pragma once

// Measured transfer throughput to each peer (v2 blueprint, claim 1: placement by bytes over
// bandwidth). The placer feeds it the payload bytes and wall time of every Push and Pull; Rank
// divides a candidate's missing bytes by Estimate() to get its transfer time.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace zeromesh::sched {

class BandwidthEstimator {
public:
    // Before any measurement: roughly a good Wi-Fi link (80 Mbit/s).
    static constexpr double kDefaultBytesPerSecond = 10'000'000.0;
    // Smaller transfers are dominated by round-trip latency, not throughput, and are ignored.
    static constexpr uint64_t kMinSampleBytes = 64 * 1024;
    // Weight of the newest sample in the exponentially weighted moving average.
    static constexpr double kAlpha = 0.3;

    void Observe(const std::string& node_id, uint64_t bytes, std::chrono::nanoseconds elapsed);

    // The EWMA for the node, or kDefaultBytesPerSecond when nothing has been measured.
    [[nodiscard]] double Estimate(const std::string& node_id) const;
    // nullopt until a sample of at least kMinSampleBytes has been seen.
    [[nodiscard]] std::optional<double> Measured(const std::string& node_id) const;

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, double> bytes_per_second_;
};

} // namespace zeromesh::sched
