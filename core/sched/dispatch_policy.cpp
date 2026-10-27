#include "sched/dispatch_policy.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <tuple>
#include <unordered_map>

namespace zeromesh::sched {

namespace {

using plugin::DispatchAssignment;
using plugin::DispatchNodeView;
using plugin::DispatchTaskView;
using plugin::IDispatchPolicy;

// Hands pending tasks out in queue order while any eligible node has capacity. A task
// no node can take right now stays queued; later tasks may still go out (e.g. a
// local-only task waiting must not hold back remote ones).
class SlotPolicy : public IDispatchPolicy {
public:
    std::vector<DispatchAssignment> Decide(const std::vector<DispatchTaskView>& pending,
                                           const std::vector<DispatchNodeView>& nodes,
                                           const std::map<std::string, std::string>& params,
                                           int job_in_flight) override {
        const int max_in_flight = ParamInt(params, "max_in_flight", 0);
        const int queue = QueueAllowance(params);
        std::unordered_map<std::string, int> added;
        std::vector<DispatchAssignment> out;
        for (const auto& t : pending) {
            if (max_in_flight > 0 && job_in_flight + static_cast<int>(out.size()) >= max_in_flight) break;
            const DispatchNodeView* best = nullptr;
            int best_free = 0;
            for (const auto& base : nodes) {
                if (!Fits(t, base)) continue;
                DispatchNodeView n = base;
                n.in_flight += added[n.node_id];
                const int free = Capacity(n, t.cores, std::min(queue, std::max(n.queue_depth, 0)));
                if (free < 1) continue;
                if (!best || Better(n, free, *best, best_free)) {
                    best = &base;
                    best_free = free;
                }
            }
            if (!best) continue;
            ++added[best->node_id];
            out.push_back({t.id, best->node_id});
        }
        return out;
    }

protected:
    // Extra tasks a donor may hold queued beyond its slots (capped by its queue_depth).
    virtual int QueueAllowance(const std::map<std::string, std::string>& /*params*/) const { return 0; }

    // Most free capacity, then fewer missing input bytes, lower load, AC, latency, id.
    virtual bool Better(const DispatchNodeView& a, int a_free, const DispatchNodeView& b, int b_free) const {
        auto key = [](const DispatchNodeView& n, int free) {
            return std::make_tuple(-free, std::max<int64_t>(n.missing_bytes, 0), n.cpu_load_pct, !n.on_ac,
                                   n.latency_ms, n.node_id);
        };
        return key(a, a_free) < key(b, b_free);
    }
};

class PullPolicy final : public SlotPolicy {
public:
    std::string_view Name() const override { return "pull"; }
};

class SpreadPolicy final : public SlotPolicy {
public:
    std::string_view Name() const override { return "spread"; }
};

class BatchPolicy final : public SlotPolicy {
public:
    std::string_view Name() const override { return "batch"; }

protected:
    int QueueAllowance(const std::map<std::string, std::string>& params) const override {
        return std::max(ParamInt(params, "prefetch", 1), 0);
    }
};

class FastPolicy final : public SlotPolicy {
public:
    std::string_view Name() const override { return "fast"; }

protected:
    int QueueAllowance(const std::map<std::string, std::string>& /*params*/) const override { return 1 << 20; }
};

class LocalFirstPolicy final : public SlotPolicy {
public:
    std::string_view Name() const override { return "local-first"; }

protected:
    bool Better(const DispatchNodeView& a, int a_free, const DispatchNodeView& b, int b_free) const override {
        if (a.is_local != b.is_local) return a.is_local;
        return SlotPolicy::Better(a, a_free, b, b_free);
    }
};

} // namespace

bool Fits(const DispatchTaskView& t, const DispatchNodeView& n) {
    if (n.is_local ? !t.may_run_local : !t.may_run_remote) return false;
    if (!t.arch.empty() && n.arch != t.arch) return false;
    if (t.mem_bytes > 0 && n.mem_available < static_cast<uint64_t>(t.mem_bytes)) return false;
    return std::find(t.exclude_nodes.begin(), t.exclude_nodes.end(), n.node_id) == t.exclude_nodes.end();
}

int Capacity(const DispatchNodeView& n, int task_cores, int queue) {
    if (n.slots > 0) return n.slots + std::max(queue, 0) - n.in_flight;
    return std::max(n.cores_total, 0) / std::max(task_cores, 1) - n.in_flight;
}

int ParamInt(const std::map<std::string, std::string>& params, const char* key, int def) {
    auto it = params.find(key);
    if (it == params.end() || it->second.empty()) return def;
    char* end = nullptr;
    const long v = std::strtol(it->second.c_str(), &end, 10);
    if (end == it->second.c_str() || *end != '\0') return def;
    return static_cast<int>(v);
}

void RegisterBuiltinDispatchPolicies(plugin::Registry& registry) {
    registry.RegisterDispatch(std::make_unique<PullPolicy>());
    registry.RegisterDispatch(std::make_unique<BatchPolicy>());
    registry.RegisterDispatch(std::make_unique<FastPolicy>());
    registry.RegisterDispatch(std::make_unique<LocalFirstPolicy>());
    registry.RegisterDispatch(std::make_unique<SpreadPolicy>());
}

IDispatchPolicy* LookupDispatchPolicy(plugin::Registry& registry, std::string_view kind) {
    return registry.Dispatch(kind.empty() ? std::string_view("pull") : kind);
}

} // namespace zeromesh::sched
