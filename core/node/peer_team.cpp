#include "node/peer_team.hpp"

#include <future>

namespace zeromesh::node {

namespace {
constexpr auto kLookupTimeout = std::chrono::milliseconds(2000);
} // namespace

PeerTeam::PeerTeam(std::shared_ptr<net::PeerTable> peers, ClientFor client_for, std::shared_ptr<store::CasStore> cas,
                   std::string self_id)
    : peers_(std::move(peers)), client_for_(std::move(client_for)), cas_(std::move(cas)), self_id_(std::move(self_id)) {}

std::vector<std::string> PeerTeam::Peers() const {
    std::vector<std::string> out;
    for (const auto& p : peers_->ListPresent()) {
        if (p.id != self_id_) out.push_back(p.id);
    }
    return out;
}

std::vector<std::pair<std::string, v1::CacheLookupReply>> PeerTeam::Lookup(const v1::CacheLookup& req) {
    std::vector<std::pair<std::string, std::future<std::optional<v1::CacheLookupReply>>>> pending;
    for (const auto& id : Peers()) {
        auto client = client_for_(id);
        if (!client) continue;
        pending.emplace_back(id, std::async(std::launch::async, [client, req] {
                                 return client->LookupCache(req, kLookupTimeout);
                             }));
    }
    std::vector<std::pair<std::string, v1::CacheLookupReply>> out;
    for (auto& [id, f] : pending) {
        if (auto reply = f.get()) out.emplace_back(id, std::move(*reply));
    }
    return out;
}

std::optional<v1::CacheLookupReply> PeerTeam::LookupAt(const std::string& node, const v1::CacheLookup& req) {
    auto client = client_for_(node);
    if (!client) return std::nullopt;
    return client->LookupCache(req, kLookupTimeout);
}

bool PeerTeam::Pull(const std::string& node, const std::vector<v1::Digest>& blobs, const std::vector<v1::Digest>& trees) {
    auto client = client_for_(node);
    return client && PullTrees(*client, *cas_, blobs, trees);
}

int PeerTeam::Forget(const v1::CacheForget& req) {
    int told = 0;
    for (const auto& id : Peers()) {
        if (auto client = client_for_(id); client && client->ForgetCache(req)) ++told;
    }
    return told;
}

} // namespace zeromesh::node
