#include "net/peer_table.hpp"

#include <algorithm>

namespace zeromesh::net {

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

} // namespace zeromesh::net
