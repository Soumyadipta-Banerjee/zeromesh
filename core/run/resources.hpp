#pragma once

// What items of a kind cost, learned from every item this node's runs finished: wall time and peak
// memory, per kind and per size class (log2 of the item's cost hint, so a small translation unit
// is not sized like a huge one). Memory is the largest of the recent observations; time is a moving
// average. An out-of-memory kill raises the memory of that class at once. Persisted as a log that is
// replayed on start and compacted when it grows.

#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace zeromesh::run {

class ResourceModel {
public:
    explicit ResourceModel(std::filesystem::path file = {});

    void Record(const std::string& kind, double cost, int64_t wall_ms, int64_t peak_mem);
    // After an OOM kill with `limit` bytes: at least half as much again from now on.
    void RaiseMemory(const std::string& kind, double cost, int64_t limit);

    // Seconds an item of this kind and size takes; nullopt with no history for the kind.
    [[nodiscard]] std::optional<double> Seconds(const std::string& kind, double cost) const;
    // Bytes to reserve for one; nullopt with no history for the kind.
    [[nodiscard]] std::optional<int64_t> Memory(const std::string& kind, double cost) const;

private:
    struct Class {
        double ewma_ms{0};
        bool have_time{false};
        std::deque<int64_t> mem; // recent peaks
        int64_t floor{0};        // raised after OOM kills
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
