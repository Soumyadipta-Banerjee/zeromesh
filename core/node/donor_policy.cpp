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
        } else if (key == "max_memory_pct") {
            if (auto v = ParseInt(value, 1, 100))
                p.max_memory_pct = *v;
            else
                bad();
        } else if (key == "allow_network") {
            if (auto v = ParseBool(value))
                p.allow_network = *v;
            else
                bad();
        } else if (key == "allow_gpu") {
            if (auto v = ParseBool(value))
                p.allow_gpu = *v;
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
        } else if (key == "drain_grace_s") {
            if (auto v = ParseInt(value, 0, 86400))
                p.drain_grace_s = *v;
            else
                bad();
        } else if (key == "freeze_ttl_s") {
            if (auto v = ParseInt(value, 0, 86400))
                p.freeze_ttl_s = *v;
            else
                bad();
        } else if (key == "depart_on_lid") {
            if (auto v = ParseBool(value))
                p.depart_on_lid = *v;
            else
                bad();
        } else if (key == "depart_on_unplug") {
            if (auto v = ParseBool(value))
                p.depart_on_unplug = *v;
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

int64_t HeldMemory(int64_t reserved, bool alone, int64_t budget) {
    reserved = std::max<int64_t>(reserved, 0);
    if (budget <= 0) return reserved;
    return alone ? budget : std::min(reserved, budget);
}

uint64_t DonorPolicy::MemoryBudget(uint64_t mem_total) const {
    const auto pct = static_cast<uint64_t>(std::clamp(max_memory_pct, 1, 100));
    const uint64_t share = mem_total / 100 * pct + mem_total % 100 * pct / 100; // exact, and no overflow
    // What the system, zmd and the owner need is mostly a fixed amount, not a share: a small machine
    // keeps kMemoryKept (half of a tiny one) whatever the share says.
    return std::min(share, mem_total - std::min(kMemoryKept, mem_total / 2));
}

int DonorPolicy::CoreLimit(int cores_total) const {
    if (max_cores >= 0) return max_cores;
    // Same idea as LocalExecutor slots: keep a spare core only when the machine has cores to spare.
    if (cores_total <= 2) return std::max(cores_total, 0);
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

namespace {

// How long a requester should wait before offering again, per passing refusal.
constexpr int kRetryCoresBusyMs = 100; // a sibling task may finish any moment
constexpr int kRetryMemoryMs = 2000;
constexpr int kRetryBatteryMs = 15000; // battery state changes slowly

} // namespace

std::optional<Refusal> DonorPolicy::Evaluate(const net::Telemetry& t, const Demand& demand, int cores_in_use,
                                             int64_t mem_promised, int gpus_in_use) const {
    if (!t.on_ac && t.battery_pct >= 0) {
        if (ShouldDrain(t)) {
            return Refusal{RefusalKind::Busy,
                           "on battery at " + std::to_string(t.battery_pct) + "%, below the drain threshold of " +
                               std::to_string(drain_below_battery) + "%",
                           kRetryBatteryMs};
        }
        if (t.battery_pct <= allow_on_battery_above) {
            return Refusal{RefusalKind::Busy,
                           "on battery at " + std::to_string(t.battery_pct) + "%; donor policy needs AC or more than " +
                               std::to_string(allow_on_battery_above) + "%",
                           kRetryBatteryMs};
        }
    }
    if (demand.network && !allow_network) {
        return Refusal{RefusalKind::Unsupported,
                       "the batch needs network access, which this node's donor policy does not allow", 0};
    }
    const std::string& arch = demand.arch;
    if (!arch.empty() && !t.arch.empty() && arch != t.arch) {
        return Refusal{RefusalKind::Unsupported, "the batch needs arch " + arch + ", this node is " + t.arch, 0};
    }
    const int need = std::max(demand.cores, 1);
    const int limit = CoreLimit(t.cores_total);
    if (need > limit) {
        return Refusal{RefusalKind::Unsupported,
                       "the batch needs " + std::to_string(need) + " cores; donor policy allows only " +
                           std::to_string(limit),
                       0};
    }
    if (cores_in_use + need > limit) {
        return Refusal{RefusalKind::Busy,
                       "the batch needs " + std::to_string(need) + " cores; donor policy allows " + std::to_string(limit) +
                           " and " + std::to_string(cores_in_use) + " are in use",
                       kRetryCoresBusyMs};
    }
    // Memory against the budget the kernel holds all work to, never what is free this second: what
    // the batch needs (or may use, under a hard limit) must fit the budget at all, and what it holds
    // must fit beside what other batches were promised.
    const int64_t budget = static_cast<int64_t>(MemoryBudget(t.mem_total));
    if (budget > 0) {
        auto mib = [](int64_t b) { return std::to_string(b >> 20) + " MiB"; };
        if (demand.mem_limit > budget) {
            return Refusal{RefusalKind::Unsupported,
                           "the batch may use up to " + mib(demand.mem_limit) + " of memory; work on this node may use " +
                               mib(budget),
                           0};
        }
        if (demand.mem_need > budget) {
            return Refusal{RefusalKind::Unsupported,
                           "the batch needs at least " + mib(demand.mem_need) + " of memory; work on this node may use " +
                               mib(budget),
                           0};
        }
        const int64_t held = HeldMemory(demand.mem_bytes, demand.alone, budget);
        const int64_t promised = std::max<int64_t>(mem_promised, 0);
        if (held > 0 && held + promised > budget) {
            return Refusal{RefusalKind::Busy,
                           (demand.alone ? "the batch must have the " + mib(budget) + " work may use to itself; "
                                         : "the batch reserves " + mib(held) + " of memory; ") +
                               mib(promised) + " of the " + mib(budget) + " work may use are promised to other batches",
                           kRetryMemoryMs};
        }
    }
    if (demand.gpus > 0) {
        if (!allow_gpu || t.gpus <= 0) {
            return Refusal{RefusalKind::Unsupported, "the batch needs a GPU, which this node does not donate", 0};
        }
        if (demand.gpus > t.gpus) {
            return Refusal{RefusalKind::Unsupported,
                           "the batch needs " + std::to_string(demand.gpus) + " GPUs; this node has " +
                               std::to_string(t.gpus),
                           0};
        }
        if (gpus_in_use + demand.gpus > t.gpus) {
            return Refusal{RefusalKind::Busy, "every GPU is in use", kRetryCoresBusyMs};
        }
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
