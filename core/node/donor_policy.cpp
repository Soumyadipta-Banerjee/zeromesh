#include "node/donor_policy.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <sstream>

namespace zeromesh::node {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

std::optional<int> ParseInt(std::string_view v, int lo, int hi) {
    int out = 0;
    auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    if (ec != std::errc{} || ptr != v.data() + v.size() || out < lo || out > hi) return std::nullopt;
    return out;
}

std::optional<bool> ParseBool(std::string_view v) {
    if (v == "true") return true;
    if (v == "false") return false;
    return std::nullopt;
}

} // namespace

DonorPolicy DonorPolicy::Parse(std::string_view text, std::ostream& warn, std::string_view origin) {
    DonorPolicy p;
    size_t line_no = 0;
    while (!text.empty()) {
        size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;

        if (size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;

        auto where = [&] { return std::string(origin) + ":" + std::to_string(line_no) + ": "; };
        size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            warn << where() << "expected `key = value`, ignored\n";
            continue;
        }
        std::string_view key = Trim(line.substr(0, eq));
        std::string_view value = Trim(line.substr(eq + 1));
        auto bad = [&] { warn << where() << "bad value for " << key << ": '" << value << "', ignored\n"; };

        if (key == "allow_on_battery_above") {
            if (auto v = ParseInt(value, 0, 100))
                p.allow_on_battery_above = *v;
            else
                bad();
        } else if (key == "max_cores") {
            if (auto v = ParseInt(value, -1, 1 << 20))
                p.max_cores = *v;
            else
                bad();
        } else if (key == "allow_network") {
            if (auto v = ParseBool(value))
                p.allow_network = *v;
            else
                bad();
        } else if (key == "drain_below_battery") {
            if (auto v = ParseInt(value, 0, 100))
                p.drain_below_battery = *v;
            else
                bad();
        } else if (key == "drain_on_user_activity") {
            if (auto v = ParseBool(value))
                p.drain_on_user_activity = *v;
            else
                bad();
        } else if (key == "drain_hold_s") {
            if (auto v = ParseInt(value, 0, 86400))
                p.drain_hold_s = *v;
            else
                bad();
        } else if (key == "comfort_target_ms") {
            if (auto v = ParseInt(value, 1, 1000))
                p.comfort_target_ms = *v;
            else
                bad();
        } else if (key == "max_step") {
            if (auto v = ParseInt(value, 0, 4))
                p.comfort_max_step = *v;
            else
                bad();
        } else if (key == "max_copies") {
            if (auto v = ParseInt(value, 1, 8))
                p.max_copies = *v;
            else
                bad();
        } else if (key == "hold_s") {
            if (auto v = ParseInt(value, 0, 86400))
                p.comfort_hold_s = *v;
            else
                bad();
        } else {
            warn << where() << "unknown key '" << key << "', ignored\n";
        }
    }
    return p;
}

DonorPolicy DonorPolicy::Load(const std::filesystem::path& config_dir) {
    const auto path = config_dir / "donor.toml";
    std::ifstream in(path, std::ios::binary);
    if (!in) return DonorPolicy{};
    std::ostringstream ss;
    ss << in.rdbuf();
    return Parse(ss.str(), std::cerr, path.string());
}

int DonorPolicy::CoreLimit(int cores_total) const {
    if (max_cores >= 0) return max_cores;
    return std::max(cores_total - 1, 0);
}

bool DonorPolicy::ShouldDrain(const net::Telemetry& t) const {
    return !t.on_ac && t.battery_pct >= 0 && t.battery_pct < drain_below_battery;
}

bool DonorPolicy::DrainCondition(const net::Telemetry& t) const {
    if (ShouldDrain(t)) return true;
    if (drain_on_user_activity && t.user_active) return true;
    return false;
}

std::optional<std::string> DonorPolicy::Refuses(const net::Telemetry& t, const v1::TaskSpec& spec,
                                                int cores_in_use) const {
    if (!t.on_ac && t.battery_pct >= 0) {
        if (ShouldDrain(t)) {
            return "on battery at " + std::to_string(t.battery_pct) + "%, below the drain threshold of " +
                   std::to_string(drain_below_battery) + "%";
        }
        if (t.battery_pct <= allow_on_battery_above) {
            return "on battery at " + std::to_string(t.battery_pct) + "%; donor policy needs AC or more than " +
                   std::to_string(allow_on_battery_above) + "%";
        }
    }
    if (spec.policy().allow_network() && !allow_network) {
        return "task needs network access, which this node's donor policy does not allow";
    }
    const std::string& arch = spec.resources().arch();
    if (!arch.empty() && !t.arch.empty() && arch != t.arch) {
        return "task needs arch " + arch + ", this node is " + t.arch;
    }
    const int need = std::max(spec.resources().cores(), 1);
    const int limit = CoreLimit(t.cores_total);
    if (cores_in_use + need > limit) {
        return "task needs " + std::to_string(need) + " cores; donor policy allows " + std::to_string(limit) + " and " +
               std::to_string(cores_in_use) + " are in use";
    }
    const int64_t mem = spec.resources().mem_bytes();
    if (mem > 0 && static_cast<uint64_t>(mem) > t.mem_available) {
        return "task needs " + std::to_string(mem) + " bytes of memory, " + std::to_string(t.mem_available) +
               " are available";
    }
    return std::nullopt;
}

DrainController::DrainController(DonorPolicy policy) : policy_(std::move(policy)) {}

void DrainController::SetPolicy(DonorPolicy policy) {
    policy_ = std::move(policy);
}

DrainController::TickResult DrainController::Tick(const net::Telemetry& t, Clock::time_point now) {
    TickResult out;
    std::string active_reason;
    if (policy_.ShouldDrain(t)) {
        active_reason = "battery at " + std::to_string(t.battery_pct) + "% (drain below " +
                        std::to_string(policy_.drain_below_battery) + "%)";
    } else if (policy_.drain_on_user_activity && t.user_active) {
        active_reason = "user activity";
    }

    if (!active_reason.empty()) {
        clear_since_.reset();
        out.entered = !draining_;
        draining_ = true;
        reason_ = std::move(active_reason);
    } else if (draining_) {
        if (!clear_since_) clear_since_ = now;
        const auto hold = std::chrono::seconds(std::max(policy_.drain_hold_s, 0));
        if (now - *clear_since_ >= hold) {
            draining_ = false;
            reason_.clear();
            clear_since_.reset();
            out.left = true;
        }
    }

    out.draining = draining_;
    out.reason = reason_;
    return out;
}

} // namespace zeromesh::node
