#pragma once

// What items of a kind cost, learned from every item this node's runs finished: wall time and peak
// memory, per kind and per size class (log2 of the item's cost hint, so a small translation unit
// is not sized like a huge one). Memory is the largest of the recent observations, with a margin;
// time is a moving average. An out-of-memory kill says what an item needed at least, and the class
// reserves that from then on. Persisted as a log that is replayed on start and compacted when it grows.

#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace zeromesh::run {

// Reserved above what an item was seen to use: peaks vary between runs of the same command.
inline constexpr double kMemoryMargin = 1.25;

// What to reserve for an item known to need `need` bytes: the margin above it, rounded up to a MiB.
inline int64_t Reserve(int64_t need) {
    constexpr int64_t kMiB = 1 << 20;
    if (need <= 0) return 0;
    const auto want = static_cast<int64_t>(static_cast<double>(need) * kMemoryMargin);
    return (want + kMiB - 1) / kMiB * kMiB;
}

class ResourceModel {
public:
    explicit ResourceModel(std::filesystem::path file = {});

    void Record(const std::string& kind, double cost, int64_t wall_ms, int64_t peak_mem);
    // An item of the class was killed for memory needing at least `bytes` (what it held, or the
    // limit it hit): reservations cover that from now on.
    void NeedsAtLeast(const std::string& kind, double cost, int64_t bytes);

    // Seconds an item of this kind and size takes; nullopt with no history for the kind.
    [[nodiscard]] std::optional<double> Seconds(const std::string& kind, double cost) const;
    // Bytes one is known to need at least: the largest recent peak, or more when one was killed
    // for memory; nullopt with no history for the kind.
    [[nodiscard]] std::optional<int64_t> Need(const std::string& kind, double cost) const;
    // Bytes to reserve for one: its need with the margin, rounded up to a MiB.
    [[nodiscard]] std::optional<int64_t> Memory(const std::string& kind, double cost) const;

private:
    struct Class {
        double ewma_ms{0};
        bool have_time{false};
        std::deque<int64_t> mem; // recent peaks
        int64_t floor{0};        // the most an item was known to need when killed for memory
    };
    static int Bucket(double cost);
    void ApplyLocked(const std::string& kind, int bucket, int64_t wall_ms, int64_t peak_mem, int64_t floor);
    // The class for (kind, bucket), else the nearest one the kind has.
    [[nodiscard]] const Class* NearestLocked(const std::string& kind, int bucket) const;
    void Append(const std::string& kind, int bucket, int64_t wall_ms, int64_t peak_mem, int64_t floor);

    std::filesystem::path file_;
    mutable std::mutex mu_;
    std::map<std::pair<std::string, int>, Class> classes_;
};

} // namespace zeromesh::run
