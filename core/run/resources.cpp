#include "run/resources.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace zeromesh::run {

namespace {

constexpr size_t kRecentPeaks = 16;
constexpr double kAlpha = 0.3;          // weight of a new duration
constexpr double kMemoryMargin = 1.25;  // reserve this much above the largest recent peak
constexpr int64_t kMiB = 1024 * 1024;

std::string Clean(std::string s) {
    std::replace(s.begin(), s.end(), '\t', ' ');
    std::replace(s.begin(), s.end(), '\n', ' ');
    return s;
}

} // namespace

ResourceModel::ResourceModel(std::filesystem::path file) : file_(std::move(file)) {
    if (file_.empty()) return;
    std::ifstream in(file_);
    std::string line;
    size_t lines = 0;
    while (std::getline(in, line)) {
        // kind \t bucket \t wall_ms \t peak_mem \t floor
        std::istringstream f(line);
        std::string kind, bucket, wall, mem, floor;
        if (!std::getline(f, kind, '\t') || !std::getline(f, bucket, '\t') || !std::getline(f, wall, '\t') ||
            !std::getline(f, mem, '\t') || !std::getline(f, floor)) {
            continue;
        }
        try {
            ApplyLocked(kind, std::stoi(bucket), std::stoll(wall), std::stoll(mem), std::stoll(floor));
            ++lines;
        } catch (const std::exception&) {
        }
    }
    if (lines > 50 * classes_.size() + 1000) {
        // Rewrite as one line per remembered observation.
        std::ofstream out(file_, std::ios::trunc);
        for (const auto& [key, c] : classes_) {
            out << key.first << '\t' << key.second << '\t' << static_cast<int64_t>(c.ewma_ms) << '\t' << 0 << '\t'
                << c.floor << '\n';
            for (int64_t m : c.mem) out << key.first << '\t' << key.second << '\t' << -1 << '\t' << m << '\t' << 0 << '\n';
        }
    }
}

int ResourceModel::Bucket(double cost) {
    if (!(cost > 1.0) || !std::isfinite(cost)) return 0;
    return 1 + static_cast<int>(std::floor(std::log2(cost)));
}

void ResourceModel::ApplyLocked(const std::string& kind, int bucket, int64_t wall_ms, int64_t peak_mem, int64_t floor) {
    Class& c = classes_[{kind, bucket}];
    if (wall_ms >= 0) {
        c.ewma_ms = c.have_time ? (1 - kAlpha) * c.ewma_ms + kAlpha * static_cast<double>(wall_ms)
                                : static_cast<double>(wall_ms);
        c.have_time = true;
    }
    if (peak_mem > 0) {
        c.mem.push_back(peak_mem);
        while (c.mem.size() > kRecentPeaks) c.mem.pop_front();
    }
    c.floor = std::max(c.floor, floor);
}

void ResourceModel::Append(const std::string& kind, int bucket, int64_t wall_ms, int64_t peak_mem, int64_t floor) {
    if (file_.empty()) return;
    std::ofstream out(file_, std::ios::app);
    out << Clean(kind) << '\t' << bucket << '\t' << wall_ms << '\t' << peak_mem << '\t' << floor << '\n';
}

void ResourceModel::Record(const std::string& kind, double cost, int64_t wall_ms, int64_t peak_mem) {
    const int b = Bucket(cost);
    std::lock_guard lock(mu_);
    ApplyLocked(kind, b, wall_ms, peak_mem, 0);
    Append(kind, b, wall_ms, peak_mem, 0);
}

void ResourceModel::RaiseMemory(const std::string& kind, double cost, int64_t limit) {
    const int b = Bucket(cost);
    const int64_t floor = std::max<int64_t>(limit + limit / 2, 256 * kMiB);
    std::lock_guard lock(mu_);
    ApplyLocked(kind, b, -1, 0, floor);
    Append(kind, b, -1, 0, floor);
}

const ResourceModel::Class* ResourceModel::NearestLocked(const std::string& kind, int bucket) const {
    const Class* best = nullptr;
    int best_dist = 0;
    for (auto it = classes_.lower_bound({kind, std::numeric_limits<int>::min()});
         it != classes_.end() && it->first.first == kind; ++it) {
        const int d = std::abs(it->first.second - bucket);
        // Prefer the larger class on a tie: over-reserving costs less than an OOM kill.
        if (!best || d < best_dist || (d == best_dist && it->first.second > bucket)) {
            best = &it->second;
            best_dist = d;
        }
    }
    return best;
}

std::optional<double> ResourceModel::Seconds(const std::string& kind, double cost) const {
    std::lock_guard lock(mu_);
    const Class* c = NearestLocked(kind, Bucket(cost));
    if (!c || !c->have_time) return std::nullopt;
    return c->ewma_ms / 1000.0;
}

std::optional<int64_t> ResourceModel::Memory(const std::string& kind, double cost) const {
    std::lock_guard lock(mu_);
    const Class* c = NearestLocked(kind, Bucket(cost));
    if (!c || (c->mem.empty() && c->floor == 0)) return std::nullopt;
    int64_t peak = c->mem.empty() ? 0 : *std::max_element(c->mem.begin(), c->mem.end());
    int64_t want = static_cast<int64_t>(static_cast<double>(peak) * kMemoryMargin);
    want = std::max(want, c->floor);
    return (want + kMiB - 1) / kMiB * kMiB;
}

} // namespace zeromesh::run
