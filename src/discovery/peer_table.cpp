#include "discovery/peer_table.hpp"

#include <algorithm>

namespace zeromesh::discovery {

void PeerTable::Upsert(Peer peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    peer.last_seen = std::chrono::steady_clock::now();
    peers_[peer.id] = std::move(peer);
}

void PeerTable::MarkDraining(const std::string& id, const std::string& reason) {
    std::vector<PreemptCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = peers_.find(id);
        if (it != peers_.end()) {
            it->second.state = PeerState::Draining;
            it->second.draining_reason = reason;
        }
        callbacks = preempt_callbacks_;
    }

    // Trigger fast preemption callbacks (< 500ms)
    for (const auto& cb : callbacks) {
        if (cb) {
            cb(id, reason);
        }
    }
}

void PeerTable::RegisterPreemptCallback(PreemptCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    preempt_callbacks_.push_back(std::move(cb));
}

void PeerTable::Remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.erase(id);
}

std::optional<Peer> PeerTable::Get(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(id);
    if (it != peers_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<Peer> PeerTable::ListActive(std::chrono::milliseconds max_age) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - max_age;

    std::vector<Peer> active;
    for (const auto& [_, peer] : peers_) {
        if (peer.last_seen >= cutoff && peer.state == PeerState::Active) {
            active.push_back(peer);
        }
    }

    std::sort(active.begin(), active.end(), [](const Peer& a, const Peer& b) {
        return a.id < b.id;
    });

    return active;
}

std::optional<Peer> PeerTable::SelectBestWorker() const {
    auto active = ListActive();
    if (active.empty()) {
        return std::nullopt;
    }

    // Filter out donors with critically low battery (< 20% and not charging)
    std::vector<Peer> candidates;
    for (const auto& peer : active) {
        if (peer.battery_pct >= 0 && peer.battery_pct < 20 && !peer.is_charging) {
            continue; // protect low-battery donor
        }
        candidates.push_back(peer);
    }

    if (candidates.empty()) {
        candidates = std::move(active);
    }

    // Sort by compute headroom: available cores weighted by idle capacity
    std::sort(candidates.begin(), candidates.end(), [](const Peer& a, const Peer& b) {
        double score_a = static_cast<double>(a.cores) * (100.0 - a.cpu_load_pct);
        double score_b = static_cast<double>(b.cores) * (100.0 - b.cpu_load_pct);
        return score_a > score_b;
    });

    return candidates.front();
}

size_t PeerTable::PruneStale(std::chrono::milliseconds max_age) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - max_age;

    size_t removed = 0;
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (it->second.last_seen < cutoff) {
            it = peers_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t PeerTable::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

} // namespace zeromesh::discovery
