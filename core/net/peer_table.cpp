#include "net/peer_table.hpp"

#include <algorithm>

namespace zeromesh::net {

void PeerTable::Upsert(Peer peer) {
    std::vector<PreemptCallback> callbacks;
    std::string drained_id;
    std::string drained_reason;
    bool entered_drain = false;
    PresenceBatch presence;
    std::vector<PresenceCallback> presence_callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peer.last_seen = std::chrono::steady_clock::now();
        auto it = peers_.find(peer.id);
        const bool was_draining = it != peers_.end() && it->second.state == PeerState::Draining;
        if (it == peers_.end() || (was_draining && peer.state != PeerState::Draining)) {
            presence.emplace_back(peer.id, PresenceEvent::Joined);
        }
        if (peer.state == PeerState::Draining && !was_draining) {
            presence.emplace_back(peer.id, PresenceEvent::Drained);
        }
        if (!presence.empty()) presence_callbacks = presence_callbacks_;
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
    NotifyPresence(presence, presence_callbacks);
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
    const double ms =
        alpha * static_cast<double>(sample_rtt.count()) + (1.0 - alpha) * static_cast<double>(prev.count());
    it->second.latency = std::chrono::milliseconds(static_cast<long long>(ms + 0.5));
}

void PeerTable::MarkDraining(const std::string& id, const std::string& reason) {
    std::vector<PreemptCallback> callbacks;
    PresenceBatch presence;
    std::vector<PresenceCallback> presence_callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = peers_.find(id);
        if (it != peers_.end()) {
            if (it->second.state != PeerState::Draining) {
                presence.emplace_back(id, PresenceEvent::Drained);
                presence_callbacks = presence_callbacks_;
            }
            it->second.state = PeerState::Draining;
            it->second.draining_reason = reason;
        }
        callbacks = preempt_callbacks_;
    }
    NotifyPresence(presence, presence_callbacks);

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

void PeerTable::RegisterPresenceCallback(PresenceCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    presence_callbacks_.push_back(std::move(cb));
}

void PeerTable::NotifyPresence(const PresenceBatch& events, const std::vector<PresenceCallback>& callbacks) const {
    for (const auto& [id, ev] : events) {
        for (const auto& cb : callbacks) {
            if (cb) cb(id, ev);
        }
    }
}

void PeerTable::Remove(const std::string& id) {
    PresenceBatch presence;
    std::vector<PresenceCallback> presence_callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = peers_.find(id);
        if (it == peers_.end()) return;
        // A drain already ended the session as censored; only an Active peer vanishing is a departure.
        if (it->second.state != PeerState::Draining) {
            presence.emplace_back(id, PresenceEvent::Left);
            presence_callbacks = presence_callbacks_;
        }
        peers_.erase(it);
    }
    NotifyPresence(presence, presence_callbacks);
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

    std::sort(active.begin(), active.end(), [](const Peer& a, const Peer& b) { return a.id < b.id; });

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

    std::sort(present.begin(), present.end(), [](const Peer& a, const Peer& b) { return a.id < b.id; });

    return present;
}

size_t PeerTable::PruneStale(std::chrono::milliseconds max_age) {
    PresenceBatch presence;
    std::vector<PresenceCallback> presence_callbacks;
    size_t removed = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto cutoff = std::chrono::steady_clock::now() - max_age;

        for (auto it = peers_.begin(); it != peers_.end();) {
            if (it->second.last_seen < cutoff) {
                if (it->second.state != PeerState::Draining) presence.emplace_back(it->first, PresenceEvent::Left);
                it = peers_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        if (!presence.empty()) presence_callbacks = presence_callbacks_;
    }
    NotifyPresence(presence, presence_callbacks);
    return removed;
}

void PeerTable::Clear() {
    PresenceBatch presence;
    std::vector<PresenceCallback> presence_callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Forgetting peers because we left or switched team says nothing about their reliability.
        for (const auto& [id, peer] : peers_) {
            if (peer.state != PeerState::Draining) presence.emplace_back(id, PresenceEvent::Drained);
        }
        if (!presence.empty()) presence_callbacks = presence_callbacks_;
        peers_.clear();
    }
    NotifyPresence(presence, presence_callbacks);
}

size_t PeerTable::Count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return peers_.size();
}

} // namespace zeromesh::net
