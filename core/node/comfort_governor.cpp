#include "node/comfort_governor.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

namespace zeromesh::node {

namespace {

// Writes to an existing control file; never creates one. False when the file is absent or
// the kernel refused the write.
bool WriteControl(const std::filesystem::path& p, const std::string& value) {
    int fd = ::open(p.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    const std::string line = value + "\n";
    const ssize_t n = ::write(fd, line.data(), line.size());
    ::close(fd);
    return n == static_cast<ssize_t>(line.size());
}

std::optional<uint64_t> ReadU64(const std::filesystem::path& p) {
    std::ifstream in(p);
    uint64_t v = 0;
    if (in >> v) return v;
    return std::nullopt;
}

std::optional<uint64_t> FieldAfter(std::string_view line, std::string_view key) {
    const size_t pos = line.find(key);
    if (pos == std::string_view::npos) return std::nullopt;
    const char* b = line.data() + pos + key.size();
    const char* e = line.data() + line.size();
    uint64_t v = 0;
    auto [ptr, ec] = std::from_chars(b, e, v);
    if (ec != std::errc{}) return std::nullopt;
    return v;
}

} // namespace

std::optional<PsiSample> ParsePsi(std::string_view text) {
    PsiSample out;
    bool saw_some = false;
    while (!text.empty()) {
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        if (line.rfind("some ", 0) == 0) {
            auto t = FieldAfter(line, "total=");
            if (!t) return std::nullopt;
            out.some_total_us = *t;
            saw_some = true;
        } else if (line.rfind("full ", 0) == 0) {
            if (auto t = FieldAfter(line, "total=")) out.full_total_us = *t;
        }
    }
    if (!saw_some) return std::nullopt;
    return out;
}

std::optional<PsiSample> ReadPsiFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return ParsePsi(ss.str());
}

std::optional<PsiTotals> ReadHostPsi() {
    auto cpu = ReadPsiFile("/proc/pressure/cpu");
    auto mem = ReadPsiFile("/proc/pressure/memory");
    auto io = ReadPsiFile("/proc/pressure/io");
    if (!cpu && !mem && !io) return std::nullopt;
    PsiTotals t;
    if (cpu) t.cpu_us = cpu->some_total_us;
    if (mem) t.mem_us = mem->some_total_us;
    if (io) t.io_us = io->some_total_us;
    return t;
}

std::optional<double> StallMeter::Observe(uint64_t total_us, Clock::time_point now) {
    std::optional<double> out;
    if (have_prev_) {
        const auto wall = std::chrono::duration_cast<std::chrono::microseconds>(now - prev_time_).count();
        if (wall > 0 && total_us >= prev_total_) {
            out = std::min(1.0, static_cast<double>(total_us - prev_total_) / static_cast<double>(wall));
        }
    }
    have_prev_ = true;
    prev_total_ = total_us;
    prev_time_ = now;
    return out;
}

ComfortConfig ComfortConfig::FromPolicy(const DonorPolicy& p) {
    ComfortConfig c;
    c.pressure_threshold = static_cast<double>(p.comfort_target_ms) / 1000.0;
    c.max_step = p.comfort_max_step;
    c.hold = std::chrono::seconds(p.comfort_hold_s);
    return c;
}

ComfortLadder::ComfortLadder(ComfortConfig cfg) : cfg_(cfg) {}

ComfortLadder::Result ComfortLadder::Update(double pressure, Clock::time_point now) {
    Result r;
    const bool high = pressure > cfg_.pressure_threshold;
    const bool calm = pressure <= cfg_.pressure_threshold / 2;
    const bool cooled = !last_change_ || now - *last_change_ >= cfg_.cooldown;

    high_count_ = high ? high_count_ + 1 : 0;
    if (calm) {
        if (!calm_since_) calm_since_ = now;
    } else {
        calm_since_.reset();
    }

    if (high_count_ >= cfg_.sustain_samples && cooled && level_ < cfg_.max_step) {
        ++level_;
        r.changed = true;
    } else if (calm_since_ && now - *calm_since_ >= cfg_.hold && cooled && level_ > 0) {
        --level_;
        r.changed = true;
        calm_since_ = now; // the next step up needs a fresh hold
    }
    if (r.changed) {
        last_change_ = now;
        high_count_ = 0;
    }
    r.level = level_;
    return r;
}

void CgroupActuator::Add(const std::filesystem::path& dir) {
    std::lock_guard lock(mu_);
    if (std::find(dirs_.begin(), dirs_.end(), dir) == dirs_.end()) dirs_.push_back(dir);
    ApplyTo(dir, level_);
}

void CgroupActuator::Remove(const std::filesystem::path& dir) {
    std::lock_guard lock(mu_);
    dirs_.erase(std::remove(dirs_.begin(), dirs_.end(), dir), dirs_.end());
    held_.erase(dir);
}

bool CgroupActuator::Hold(const std::filesystem::path& dir, bool frozen) {
    std::lock_guard lock(mu_);
    if (frozen)
        held_.insert(dir);
    else
        held_.erase(dir);
    const bool freeze = frozen || level_ >= 4;
    const bool ok = WriteControl(dir / "cgroup.freeze", freeze ? "1" : "0");
    if (ok) caps_.freeze = true;
    return ok;
}

void CgroupActuator::Apply(int level) {
    std::lock_guard lock(mu_);
    level_ = std::clamp(level, 0, 4);
    for (const auto& d : dirs_) ApplyTo(d, level_);
}

int CgroupActuator::level() const {
    std::lock_guard lock(mu_);
    return level_;
}

CgroupActuator::Caps CgroupActuator::Capabilities() const {
    std::lock_guard lock(mu_);
    return caps_;
}

void CgroupActuator::ApplyTo(const std::filesystem::path& dir, int level) {
    // Thaw first when stepping out of the freeze so the other limits take effect on a runnable task.
    const bool held = held_.contains(dir); // a departure freeze outranks the ladder
    if (level < 4 && !held) caps_.freeze = WriteControl(dir / "cgroup.freeze", "0") || caps_.freeze;

    const bool squeezed = level >= 1;
    const bool w = WriteControl(dir / "cpu.weight", squeezed ? "1" : "100");
    const bool i = WriteControl(dir / "cpu.idle", squeezed ? "1" : "0");
    caps_.cpu = w || i;

    if (level >= 2) {
        const uint64_t current = ReadU64(dir / "memory.current").value_or(0);
        const uint64_t high = std::max(current / 2, kMemoryHighFloor);
        caps_.memory = WriteControl(dir / "memory.high", std::to_string(high));
        if (caps_.memory && current > 0) {
            // One bounded reclaim; the kernel may reclaim less (EAGAIN), which is fine.
            WriteControl(dir / "memory.reclaim", std::to_string(std::min(current / 4, kReclaimBytes)));
        }
    } else {
        caps_.memory = WriteControl(dir / "memory.high", "max");
    }

    caps_.io = WriteControl(dir / "io.weight", level >= 3 ? "default 1" : "default 100");

    if (level >= 4 || held) caps_.freeze = WriteControl(dir / "cgroup.freeze", "1");
}

ComfortGovernor::ComfortGovernor(ComfortConfig cfg) : cfg_(cfg), ladder_(cfg) {}

ComfortGovernor::~ComfortGovernor() {
    Stop();
}

int ComfortGovernor::level() const {
    std::lock_guard lock(mu_);
    return ladder_.level();
}

void ComfortGovernor::Tick(const PsiTotals& totals, Clock::time_point now) {
    std::lock_guard lock(mu_);
    // Always feed all three meters so each has a baseline.
    const auto c = cpu_.Observe(totals.cpu_us, now);
    const auto m = mem_.Observe(totals.mem_us, now);
    const auto o = io_.Observe(totals.io_us, now);
    if (!c && !m && !o) return;
    const double worst = std::max({c.value_or(0.0), m.value_or(0.0), o.value_or(0.0)});
    const auto r = ladder_.Update(worst, now);
    if (r.changed) actuator_.Apply(r.level);
}

void ComfortGovernor::Start(std::function<std::optional<PsiTotals>()> source) {
    if (thread_.joinable()) return;
    if (!source || !source()) return; // no PSI on this kernel
    thread_ = std::jthread([this, source = std::move(source)](std::stop_token st) {
        while (!st.stop_requested()) {
            if (auto t = source()) Tick(*t, Clock::now());
            std::this_thread::sleep_for(cfg_.sample_interval);
        }
    });
}

void ComfortGovernor::Stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

} // namespace zeromesh::node
