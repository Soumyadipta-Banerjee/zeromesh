#include "sched/hazard.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace zeromesh::sched {

namespace {

constexpr double kAliveLogEveryS = 60.0;
constexpr int kExpectedMinSteps = 128;

std::string EscapeId(const std::string& id) {
    std::string out;
    for (const unsigned char c : id) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '%' || c < 0x20) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string UnescapeId(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

// ---- Curve ----

double HazardModel::Curve::Eval(double at, double exposure) const {
    if (at <= 0.0) return 0.0;
    const double tail = exposure > 0.0 ? static_cast<double>(events) / exposure : 0.0;
    if (t.empty() || at > t.back()) {
        const double t_last = t.empty() ? 0.0 : t.back();
        const double h_last = h.empty() ? 0.0 : h.back();
        return h_last + tail * (at - t_last);
    }
    const size_t i = static_cast<size_t>(std::lower_bound(t.begin(), t.end(), at) - t.begin());
    const double t0 = i == 0 ? 0.0 : t[i - 1];
    const double h0 = i == 0 ? 0.0 : h[i - 1];
    if (t[i] <= t0) return h[i];
    return h0 + (at - t0) / (t[i] - t0) * (h[i] - h0);
}

void HazardModel::Rebuild(Curve& c, const std::vector<Obs>& obs) {
    c.t.clear();
    c.h.clear();
    c.events = 0;
    c.closed_exposure = 0.0;
    std::vector<Obs> sorted = obs;
    std::sort(sorted.begin(), sorted.end(), [](const Obs& a, const Obs& b) { return a.duration_s < b.duration_s; });
    for (const Obs& o : sorted) {
        c.closed_exposure += o.duration_s;
        if (o.departed) ++c.events;
    }
    double cum = 0.0;
    size_t i = 0;
    while (i < sorted.size()) {
        const double at = sorted[i].duration_s;
        const double at_risk = static_cast<double>(sorted.size() - i); // sessions with duration >= at
        size_t j = i;
        size_t deaths = 0;
        while (j < sorted.size() && sorted[j].duration_s == at) {
            if (sorted[j].departed) ++deaths;
            ++j;
        }
        if (deaths > 0) {
            cum += static_cast<double>(deaths) / at_risk;
            c.t.push_back(at);
            c.h.push_back(cum);
        }
        i = j;
    }
}

// ---- Estimator: one query's view of the shrunk cumulative hazard ----

struct HazardModel::Estimator {
    const Curve* pool{nullptr};
    double pool_exposure{0.0};
    double default_rate_per_s{0.0};
    const Curve* node{nullptr}; // null = node has no closed sessions
    double node_exposure{0.0};
    double weight{0.0};
    double age_s{0.0};

    [[nodiscard]] double H(double at) const {
        if (at <= 0.0) return 0.0;
        const double hp = pool_exposure > 0.0 ? pool->Eval(at, pool_exposure) : default_rate_per_s * at;
        if (node == nullptr) return hp;
        return weight * node->Eval(at, node_exposure) + (1.0 - weight) * hp;
    }
    // P(survive another h | already present for age_s)
    [[nodiscard]] double S(double h) const {
        if (h <= 0.0) return 1.0;
        const double d = H(age_s + h) - H(age_s);
        return std::exp(-std::max(d, 0.0));
    }
};

HazardModel::Estimator HazardModel::MakeEstimatorLocked(const std::string& id) const {
    const double now = Now();
    Estimator e;
    e.pool = &PoolLocked();
    e.pool_exposure = e.pool->closed_exposure;
    for (const auto& [_, n] : nodes_) {
        if (n.open_since) e.pool_exposure += std::max(0.0, now - *n.open_since);
    }
    e.default_rate_per_s = cfg_.default_hazard_per_hour / 3600.0;
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return e;
    Node& n = it->second;
    if (n.open_since) e.age_s = std::max(0.0, now - *n.open_since);
    if (n.obs.empty()) return e;
    if (n.dirty) {
        Rebuild(n.curve, n.obs);
        n.dirty = false;
    }
    e.node = &n.curve;
    e.node_exposure = n.curve.closed_exposure + e.age_s;
    e.weight = e.node_exposure / (e.node_exposure + std::max(cfg_.prior_exposure_s, 1e-9));
    return e;
}

const HazardModel::Curve& HazardModel::PoolLocked() const {
    if (pool_dirty_) {
        std::vector<Obs> all;
        for (const auto& [_, n] : nodes_) all.insert(all.end(), n.obs.begin(), n.obs.end());
        Rebuild(pool_, all);
        pool_dirty_ = false;
    }
    return pool_;
}

// ---- construction, events, persistence ----

HazardModel::HazardModel(std::filesystem::path log_path, Config cfg, Clock clock)
    : cfg_(cfg), clock_(std::move(clock)), path_(std::move(log_path)) {
    if (path_.empty()) return;
    std::lock_guard lock(mu_);
    ReplayLocked();
}

double HazardModel::Now() const {
    if (clock_) return clock_();
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void HazardModel::CloseLocked(Node& n, double end_s, bool departed) {
    const double start = *n.open_since;
    n.open_since.reset();
    n.last_alive = -1.0;
    n.obs.push_back({std::max(end_s - start, cfg_.min_session_s), departed});
    if (n.obs.size() > cfg_.max_obs_per_node) {
        n.obs.erase(n.obs.begin(), n.obs.begin() + static_cast<std::ptrdiff_t>(n.obs.size() - cfg_.max_obs_per_node));
    }
    n.dirty = true;
    pool_dirty_ = true;
}

bool HazardModel::ApplyLocked(char op, const std::string& id, double t) {
    Node& n = nodes_[id];
    switch (op) {
    case 'J':
        if (n.open_since) {
            // We never saw the previous session end. Count it as a departure only if we
            // have a later sign of life to date it by; otherwise its fate is unknown.
            if (n.last_alive > *n.open_since) {
                CloseLocked(n, n.last_alive, true);
            } else {
                n.open_since.reset();
                n.last_alive = -1.0;
            }
        }
        n.open_since = t;
        n.last_alive = -1.0;
        n.last_alive_written = -1.0e18;
        return true;
    case 'L':
    case 'D':
        if (!n.open_since) return false;
        CloseLocked(n, t, op == 'L');
        return true;
    case 'A':
        if (!n.open_since || t <= n.last_alive) return false;
        n.last_alive = t;
        if (t - n.last_alive_written < kAliveLogEveryS) return false;
        n.last_alive_written = t;
        return true;
    default:
        return false;
    }
}

void HazardModel::AppendLocked(char op, const std::string& id, double t) {
    if (path_.empty()) return;
    std::error_code ec;
    if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), ec);
    std::ofstream out(path_, std::ios::app);
    if (!out) return; // best effort: a read-only state dir must not break scheduling
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", t);
    out << op << ' ' << EscapeId(id) << ' ' << buf << '\n';
}

void HazardModel::ReplayLocked() {
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string op, id;
        double t = 0.0;
        if (!(ls >> op >> id >> t) || op.size() != 1 || !std::isfinite(t)) continue;
        if (op[0] != 'J' && op[0] != 'L' && op[0] != 'D' && op[0] != 'A') continue;
        const std::string node = UnescapeId(id);
        if (op[0] == 'A') {
            // Replayed heartbeats were already throttled when written.
            auto it = nodes_.find(node);
            if (it != nodes_.end() && it->second.open_since && t > it->second.last_alive) {
                it->second.last_alive = t;
                it->second.last_alive_written = t;
            }
            continue;
        }
        ApplyLocked(op[0], node, t);
    }
}

void HazardModel::NoteJoin(const std::string& id, double t_s) {
    std::lock_guard lock(mu_);
    if (ApplyLocked('J', id, t_s)) AppendLocked('J', id, t_s);
}

void HazardModel::NoteLeave(const std::string& id, double t_s) {
    std::lock_guard lock(mu_);
    if (ApplyLocked('L', id, t_s)) AppendLocked('L', id, t_s);
}

void HazardModel::NoteDrain(const std::string& id, double t_s) {
    std::lock_guard lock(mu_);
    if (ApplyLocked('D', id, t_s)) AppendLocked('D', id, t_s);
}

void HazardModel::NoteAlive(const std::string& id, double t_s) {
    std::lock_guard lock(mu_);
    if (ApplyLocked('A', id, t_s)) AppendLocked('A', id, t_s);
}

// ---- queries ----

double HazardModel::SurvivalProb(const std::string& id, double horizon_s) const {
    std::lock_guard lock(mu_);
    return MakeEstimatorLocked(id).S(horizon_s);
}

double HazardModel::HazardPerHour(const std::string& id) const {
    std::lock_guard lock(mu_);
    const double tau = std::max(cfg_.reference_horizon_s, 1.0);
    return 3600.0 * MakeEstimatorLocked(id).H(tau) / tau;
}

double HazardModel::ExpectedMin(const std::string& id, double task_s) const {
    if (!(task_s > 0.0)) return 0.0;
    std::lock_guard lock(mu_);
    const Estimator e = MakeEstimatorLocked(id);
    // integral_0^T S(h) dh by the trapezoid rule; S is smooth enough that 128 steps is ample.
    const double dt = task_s / kExpectedMinSteps;
    double sum = 0.5 * (e.S(0.0) + e.S(task_s));
    for (int i = 1; i < kExpectedMinSteps; ++i) sum += e.S(dt * i);
    return std::min(sum * dt, task_s);
}

bool HazardModel::HasHistory() const {
    std::lock_guard lock(mu_);
    for (const auto& [_, n] : nodes_) {
        if (!n.obs.empty()) return true;
    }
    return false;
}

size_t HazardModel::Observations(const std::string& id) const {
    std::lock_guard lock(mu_);
    auto it = nodes_.find(id);
    return it == nodes_.end() ? 0 : it->second.obs.size();
}

size_t HazardModel::Events(const std::string& id) const {
    std::lock_guard lock(mu_);
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return 0;
    return static_cast<size_t>(
        std::count_if(it->second.obs.begin(), it->second.obs.end(), [](const Obs& o) { return o.departed; }));
}

HazardModel::SurvivalFn HazardModel::MakeSurvivalFn(std::shared_ptr<const HazardModel> model) {
    return [model = std::move(model)](const std::string& id, double horizon_s) {
        return model->SurvivalProb(id, horizon_s);
    };
}

void ObservePeers(net::PeerTable& table, std::shared_ptr<HazardModel> model) {
    table.RegisterPresenceCallback([model = std::move(model)](const std::string& id, net::PresenceEvent ev) {
        switch (ev) {
        case net::PresenceEvent::Joined:
            model->NoteJoin(id);
            break;
        case net::PresenceEvent::Left:
            model->NoteLeave(id);
            break;
        case net::PresenceEvent::Drained:
            model->NoteDrain(id);
            break;
        }
    });
}

} // namespace zeromesh::sched
