#include "net/peer_table.hpp"

#include <algorithm>

namespace zeromesh::net {

void PeerTable::Upsert(Peer peer) {
    std::vector<PreemptCallback> callbacks;
    std::string drained_id;
    std::string drained_reason;
    bool entered_drain = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer.last_seen = std::chrono::steady_clock::now();
        auto it = peers_.find(peer.id);
        const bool was_draining = it != peers_.end() && it->second.state == PeerState::Draining;
        if (peer.state == PeerState::Draining && !was_draining) {
            entered_drain = true;
            drained_id = peer.id;
            drained_reason = peer.draining_reason.empty() ? "draining" : peer.draining_reason;
            callbacks = preempt_callbacks_;
        }
        if (it != peers_.end()) {
            // Discovery always sends a placeholder latency; keep ping measurements.
            peer.latency = it->second.latency;
        }
        peers_[peer.id] = std::move(peer);
    }
    // Announcements with state=Draining must preempt the same way as ZM2D / NodeApi.Drain.
    if (entered_drain) {
        for (const auto& cb : callbacks) {
            if (cb) cb(drained_id, drained_reason);
        }
    }
}

void PeerTable::RecordMeasuredLatency(const std::string& id, std::chrono::milliseconds sample_rtt) {
    if (sample_rtt <= std::chrono::milliseconds(0)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = peers_.find(id);
    if (it == peers_.end()) return;
    const auto prev = it->second.latency;
    if (prev <= std::chrono::milliseconds(0)) {
        it->second.latency = sample_rtt;
        return;
    }
    // EWMA with alpha = 0.25
    const double alpha = 0.25;
    const double ms = alpha * static_cast<double>(sample_rtt.count()) +
                      (1.0 - alpha) * static_cast<double>(prev.count());
    it->second.latency = std::chrono::milliseconds(static_cast<long long>(ms + 0.5));
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

std::vector<Peer> PeerTable::ListPresent(std::chrono::milliseconds max_age) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto cutoff = std::chrono::steady_clock::now() - max_age;

    std::vector<Peer> present;
    for (const auto& [_, peer] : peers_) {
        if (peer.last_seen < cutoff) continue;
        if (peer.state != PeerState::Active && peer.state != PeerState::Draining) continue;
        present.push_back(peer);
    }

    std::sort(present.begin(), present.end(), [](const Peer& a, const Peer& b) {
        return a.id < b.id;
    });

    return present;
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

void PeerTable::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    peers_.clear();
}

size_t PeerTable::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

} // namespace zeromesh::net
