#pragma once

// Per-node departure hazard (claim 3; SkyNomad-style). Every join and leave of a team
// peer is logged, and a Nelson-Aalen cumulative-hazard estimate is fitted per node:
//   - an abrupt departure (peer vanished, heartbeat timeout) is an event;
//   - a graceful drain (unplug, lid, owner, or us leaving the team) is right-censored: the
//     node did not fail, we just stopped observing it.
// With only 5-20 laptops a node has few samples, so its curve is shrunk toward a curve
// pooled over all nodes: H(t) = w * H_node(t) + (1 - w) * H_pool(t), with
// w = exposure_node / (exposure_node + prior_exposure_s). An empty pool falls back to a
// constant default hazard.
//
// Estimator details:
//   - H_node is the Nelson-Aalen step curve over closed sessions, linearly interpolated
//     between steps (H(0) = 0), and extrapolated past the last event at the constant rate
//     events / exposure. Exposure includes the age of sessions still open.
//   - Open sessions are not in the NA risk set; their age conditions SurvivalProb on the
//     time already survived: S(h) = exp(-(H(age + h) - H(age))).
//   - HazardPerHour(node) = 3600 * H(reference_horizon_s) / reference_horizon_s.
//
// History is appended to a text log (one line per event: "J|L|D|A <node> <seconds>") and
// replayed on construction, so it survives restarts. A session still open when the log is
// reloaded has unknown fate and is dropped (unless NoteAlive recorded a later sign of life,
// in which case it counts as a departure at that time when the node joins again).

#include "net/peer_table.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace zeromesh::sched {

struct HazardConfig {
    // Shrinkage strength: a node with this much observed time counts half as much as the pool.
    double prior_exposure_s{8 * 3600.0};
    // Used while no node has any closed session.
    double default_hazard_per_hour{0.25};
    // Horizon at which HazardPerHour reads the cumulative hazard.
    double reference_horizon_s{3600.0};
    // Closed sessions kept per node (oldest dropped first).
    size_t max_obs_per_node{1000};
    // Shortest session recorded; flapping below this is clamped up to it.
    double min_session_s{1.0};
};

class HazardModel {
public:
    using Config = HazardConfig;
    using Clock = std::function<double()>; // seconds since the epoch
    using SurvivalFn = std::function<double(const std::string& node_id, double horizon_s)>;

    // `log_path` empty = in-memory only. Otherwise the file (and its directory) is created on
    // demand and replayed now. `clock` defaults to the wall clock.
    explicit HazardModel(std::filesystem::path log_path = {}, Config cfg = {}, Clock clock = {});

    // The node appeared / vanished without warning / left gracefully. The overloads taking a
    // time are for replay and tests; the others use the clock. A leave or drain for a node
    // with no open session is ignored.
    void NoteJoin(const std::string& id) { NoteJoin(id, Now()); }
    void NoteJoin(const std::string& id, double t_s);
    void NoteLeave(const std::string& id) { NoteLeave(id, Now()); }
    void NoteLeave(const std::string& id, double t_s);
    void NoteDrain(const std::string& id) { NoteDrain(id, Now()); }
    void NoteDrain(const std::string& id, double t_s);
    // A sign of life for an open session (persisted at most once a minute per node).
    void NoteAlive(const std::string& id) { NoteAlive(id, Now()); }
    void NoteAlive(const std::string& id, double t_s);

    // P(the node is still present `horizon_s` from now), given how long it has been present.
    [[nodiscard]] double SurvivalProb(const std::string& id, double horizon_s) const;
    // Cumulative hazard over the reference horizon, expressed per hour.
    [[nodiscard]] double HazardPerHour(const std::string& id) const;
    // E[min(remaining life, task_s)] in seconds; at most task_s.
    [[nodiscard]] double ExpectedMin(const std::string& id, double task_s) const;

    // True once any node has a closed session; before that every answer is the default.
    [[nodiscard]] bool HasHistory() const;
    [[nodiscard]] size_t Observations(const std::string& id) const; // closed sessions
    [[nodiscard]] size_t Events(const std::string& id) const;       // of those, abrupt departures

    // Injectable survival function for consumers such as a hedging agent. Keeps the model alive.
    [[nodiscard]] static SurvivalFn MakeSurvivalFn(std::shared_ptr<const HazardModel> model);

private:
    struct Obs {
        double duration_s;
        bool departed;
    };
    struct Curve {
        std::vector<double> t; // event times, ascending, distinct
        std::vector<double> h; // cumulative hazard at t
        size_t events{0};
        double closed_exposure{0.0};
        [[nodiscard]] double Eval(double at, double exposure) const;
    };
    struct Node {
        std::vector<Obs> obs;
        std::optional<double> open_since;
        double last_alive{-1.0};
        double last_alive_written{-1.0e18};
        Curve curve;
        bool dirty{true};
    };
    struct Estimator;

    [[nodiscard]] double Now() const;
    // Returns true when the event changed state and should be logged.
    bool ApplyLocked(char op, const std::string& id, double t);
    void AppendLocked(char op, const std::string& id, double t);
    void ReplayLocked();
    void CloseLocked(Node& n, double end_s, bool departed);
    static void Rebuild(Curve& c, const std::vector<Obs>& obs);
    const Curve& PoolLocked() const;
    [[nodiscard]] Estimator MakeEstimatorLocked(const std::string& id) const;

    Config cfg_;
    Clock clock_;
    std::filesystem::path path_;
    mutable std::mutex mu_;
    mutable std::unordered_map<std::string, Node> nodes_;
    mutable Curve pool_;
    mutable bool pool_dirty_{true};
};

// Feeds a PeerTable's presence events into `model`: a new peer is a join, a peer that
// disappears while Active (removed, pruned) is a departure, a drain or leaving the team is
// a censored end of session.
void ObservePeers(net::PeerTable& table, std::shared_ptr<HazardModel> model);

} // namespace zeromesh::sched
