#include "net/telemetry.hpp"

#include <sys/utsname.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

namespace zeromesh::net {

namespace {

namespace fs = std::filesystem;

std::string ReadTrimmed(const fs::path& p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
}

struct CpuTimes {
    uint64_t total{0};
    uint64_t idle{0};
    int cores{0};
};

// Aggregate "cpu" line: user nice system idle iowait irq softirq steal [guest guest_nice].
// guest time is already included in user, so only the first eight fields are summed.
CpuTimes ReadProcStat(const fs::path& path) {
    CpuTimes t;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("cpu", 0) != 0) continue;
        std::istringstream ls(line);
        std::string label;
        ls >> label;
        if (label == "cpu") {
            uint64_t v = 0;
            for (int i = 0; i < 8 && (ls >> v); ++i) {
                t.total += v;
                if (i == 3 || i == 4) t.idle += v;  // idle + iowait
            }
        } else {
            ++t.cores;
        }
    }
    return t;
}

void ReadMeminfo(const fs::path& path, Telemetry& out) {
    std::ifstream in(path);
    std::string key;
    uint64_t kb = 0;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        if (!(ls >> key >> kb)) continue;
        if (key == "MemTotal:") out.mem_total = kb * 1024;
        else if (key == "MemAvailable:") out.mem_available = kb * 1024;
    }
}

void ReadPowerSupply(const fs::path& dir, Telemetry& out) {
    std::error_code ec;
    std::vector<fs::path> supplies;
    for (const auto& entry : fs::directory_iterator(dir, ec)) supplies.push_back(entry.path());
    std::sort(supplies.begin(), supplies.end());

    bool have_battery = false;
    bool mains_online = false;
    for (const auto& s : supplies) {
        const std::string type = ReadTrimmed(s / "type");
        if (type == "Battery" && !have_battery) {
            const std::string cap = ReadTrimmed(s / "capacity");
            if (!cap.empty()) {
                try {
                    out.battery_pct = std::clamp(std::stoi(cap), 0, 100);
                    have_battery = true;
                } catch (const std::exception&) {
                }
            }
        } else if (type == "Mains" && ReadTrimmed(s / "online") == "1") {
            mains_online = true;
        }
    }
    out.on_ac = mains_online || !have_battery;
}

bool InputIrqLine(const std::string& lower) {
    static constexpr const char* kMarkers[] = {
        "i8042", "keyboard", "mouse", "touchpad", "i2c_designware", "xhci_hcd", "hid",
        "gpio-keys", "elan", "synaptics",
    };
    for (const char* m : kMarkers) {
        if (lower.find(m) != std::string::npos) return true;
    }
    return false;
}

// Sum of IRQ counts whose device name looks like keyboard/mouse/touchpad/USB input.
uint64_t ReadInputIrqs(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    uint64_t total = 0;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string lower = line;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!InputIrqLine(lower)) continue;
        std::istringstream ls(line.substr(colon + 1));
        uint64_t n = 0;
        while (ls >> n) total += n;
    }
    return total;
}

std::optional<bool> ReadUserActiveOverride(const fs::path& path) {
    if (!fs::exists(path)) return std::nullopt;
    const std::string v = ReadTrimmed(path);
    if (v == "1" || v == "true") return true;
    if (v == "0" || v == "false") return false;
    return std::nullopt;
}

}  // namespace

TelemetrySampler::TelemetrySampler(std::filesystem::path sysroot) : sysroot_(std::move(sysroot)) {}

Telemetry TelemetrySampler::Sample() {
    Telemetry t;

    const CpuTimes cpu = ReadProcStat(sysroot_ / "proc/stat");
    t.cores_total = cpu.cores > 0 ? cpu.cores : static_cast<int>(std::thread::hardware_concurrency());
    if (have_prev_ && cpu.total > prev_total_ && cpu.idle >= prev_idle_) {
        const double dtotal = static_cast<double>(cpu.total - prev_total_);
        const double didle = static_cast<double>(cpu.idle - prev_idle_);
        t.cpu_load_pct = std::clamp(100.0 * (dtotal - didle) / dtotal, 0.0, 100.0);
    }
    have_prev_ = true;
    prev_total_ = cpu.total;
    prev_idle_ = cpu.idle;
    t.cores_free = static_cast<int>(std::lround(t.cores_total * (1.0 - t.cpu_load_pct / 100.0)));

    ReadMeminfo(sysroot_ / "proc/meminfo", t);
    ReadPowerSupply(sysroot_ / "sys/class/power_supply", t);

    if (auto over = ReadUserActiveOverride(sysroot_ / "run/zeromesh/user_active")) {
        t.user_active = *over;
    } else {
        // Require a clear jump in input IRQs so background noise and test harness
        // activity do not flap the donor into sticky drain.
        const uint64_t irqs = ReadInputIrqs(sysroot_ / "proc/interrupts");
        constexpr uint64_t kActivityDelta = 8;
        if (have_irq_prev_ && irqs > prev_input_irqs_ &&
            (irqs - prev_input_irqs_) >= kActivityDelta) {
            t.user_active = true;
        }
        have_irq_prev_ = true;
        prev_input_irqs_ = irqs;
    }

    utsname u{};
    if (uname(&u) == 0) t.arch = u.machine;
    return t;
}

}  // namespace zeromesh::net
