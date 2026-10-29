#include "node/work_pressure.hpp"

#include "node/comfort_governor.hpp"

namespace zeromesh::node {

WorkPressure::WorkPressure(std::filesystem::path work_cgroup, std::function<bool()> stop_youngest,
                           WorkPressureConfig cfg)
    : stop_youngest_(std::move(stop_youngest)), cfg_(cfg) {
    if (work_cgroup.empty()) return;
    pressure_file_ = work_cgroup / "memory.pressure";
    thread_ = std::jthread([this](std::stop_token st) { Loop(st); });
}

WorkPressure::~WorkPressure() {
    thread_.request_stop();
    cv_.notify_all();
}

bool WorkPressure::Observe(double full_avg10, Clock::time_point now) {
    if (full_avg10 < cfg_.stalled_pct) return false;
    if (last_stop_ && now - *last_stop_ < cfg_.settle) return false;
    if (!stop_youngest_ || !stop_youngest_()) return false;
    last_stop_ = now;
    return true;
}

void WorkPressure::Loop(std::stop_token st) {
    std::unique_lock lock(mu_);
    while (!st.stop_requested()) {
        if (auto sample = ReadPsiFile(pressure_file_)) Observe(sample->full_avg10, Clock::now());
        cv_.wait_for(lock, st, cfg_.interval, [] { return false; });
    }
}

} // namespace zeromesh::node
