#include "dht/local_store.hpp"

#include <algorithm>

namespace zeromesh::dht {

// ─────────────────────────────────────────────────────────────────────────────
// Value Operations
// ─────────────────────────────────────────────────────────────────────────────

bool LocalStore::PutValue(const Key& key, const std::string& value,
                          const NodeId& publisher_id,
                          std::chrono::seconds ttl) {
    if (value.size() > kMaxValueSize) {
        return false;
    }

    auto now = std::chrono::steady_clock::now();
    ValueEntry entry{};
    entry.key = key;
    entry.value = value;
    entry.publisher_id = publisher_id;
    entry.stored_at = now;
    entry.expires_at = now + ttl;

    std::lock_guard<std::mutex> lock(value_mutex_);
    values_[key] = std::move(entry);
    return true;
}

std::optional<std::string> LocalStore::GetValue(const Key& key) const {
    std::lock_guard<std::mutex> lock(value_mutex_);
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;

    // Check expiration.
    if (std::chrono::steady_clock::now() >= it->second.expires_at) {
        return std::nullopt;
    }

    return it->second.value;
}

bool LocalStore::HasValue(const Key& key) const {
    std::lock_guard<std::mutex> lock(value_mutex_);
    auto it = values_.find(key);
    if (it == values_.end()) return false;
    return std::chrono::steady_clock::now() < it->second.expires_at;
}

bool LocalStore::RemoveValue(const Key& key) {
    std::lock_guard<std::mutex> lock(value_mutex_);
    return values_.erase(key) > 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Provider Operations
// ─────────────────────────────────────────────────────────────────────────────

void LocalStore::AddProvider(const Key& chunk_hash, const PeerContact& provider) {
    auto now = std::chrono::steady_clock::now();

    ProviderEntry entry{};
    entry.contact = provider;
    entry.added_at = now;
    entry.expires_at = now + kProviderTtl;

    std::lock_guard<std::mutex> lock(provider_mutex_);
    auto& providers = providers_[chunk_hash];

    // If this provider is already registered, refresh its timestamp.
    for (auto& existing : providers) {
        if (existing.contact.id == provider.id) {
            existing.added_at = now;
            existing.expires_at = now + kProviderTtl;
            existing.contact.ip_address = provider.ip_address;
            existing.contact.port = provider.port;
            return;
        }
    }

    providers.push_back(std::move(entry));
}

std::vector<PeerContact> LocalStore::GetProviders(const Key& chunk_hash) const {
    std::lock_guard<std::mutex> lock(provider_mutex_);
    auto it = providers_.find(chunk_hash);
    if (it == providers_.end()) return {};

    auto now = std::chrono::steady_clock::now();
    std::vector<PeerContact> result;
    for (const auto& entry : it->second) {
        if (now < entry.expires_at) {
            result.push_back(entry.contact);
        }
    }
    return result;
}

bool LocalStore::RemoveProvider(const Key& chunk_hash, const NodeId& provider_id) {
    std::lock_guard<std::mutex> lock(provider_mutex_);
    auto it = providers_.find(chunk_hash);
    if (it == providers_.end()) return false;

    auto& vec = it->second;
    auto rm = std::remove_if(vec.begin(), vec.end(),
                             [&](const ProviderEntry& e) {
                                 return e.contact.id == provider_id;
                             });
    if (rm == vec.end()) return false;

    vec.erase(rm, vec.end());
    if (vec.empty()) {
        providers_.erase(it);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Maintenance
// ─────────────────────────────────────────────────────────────────────────────

size_t LocalStore::PurgeExpired() {
    size_t evicted = 0;
    auto now = std::chrono::steady_clock::now();

    // Purge expired values.
    {
        std::lock_guard<std::mutex> lock(value_mutex_);
        for (auto it = values_.begin(); it != values_.end(); ) {
            if (now >= it->second.expires_at) {
                it = values_.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
    }

    // Purge expired provider entries.
    {
        std::lock_guard<std::mutex> lock(provider_mutex_);
        for (auto it = providers_.begin(); it != providers_.end(); ) {
            auto& vec = it->second;
            auto rm = std::remove_if(vec.begin(), vec.end(),
                                     [&](const ProviderEntry& e) {
                                         return now >= e.expires_at;
                                     });
            evicted += static_cast<size_t>(std::distance(rm, vec.end()));
            vec.erase(rm, vec.end());

            if (vec.empty()) {
                it = providers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    return evicted;
}

size_t LocalStore::ValueCount() const {
    std::lock_guard<std::mutex> lock(value_mutex_);
    return values_.size();
}

size_t LocalStore::ProviderKeyCount() const {
    std::lock_guard<std::mutex> lock(provider_mutex_);
    return providers_.size();
}

std::vector<ValueEntry> LocalStore::EntriesNeedingRepublish() const {
    std::lock_guard<std::mutex> lock(value_mutex_);
    auto now = std::chrono::steady_clock::now();
    std::vector<ValueEntry> result;

    for (const auto& [key, entry] : values_) {
        // Republish entries that have passed the midpoint of their TTL.
        auto total_ttl = entry.expires_at - entry.stored_at;
        auto midpoint = entry.stored_at + total_ttl / 2;
        if (now >= midpoint && now < entry.expires_at) {
            result.push_back(entry);
        }
    }
    return result;
}

} // namespace zeromesh::dht
