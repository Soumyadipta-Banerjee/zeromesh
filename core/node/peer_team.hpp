#pragma once

// run::Team over this node's admitted active peers.

#include "net/peer_table.hpp"
#include "node/node_client.hpp"
#include "run/team.hpp"
#include "store/store.hpp"

#include <functional>
#include <memory>
#include <string>

namespace zeromesh::node {

class PeerTeam final : public run::Team {
public:
    using ClientFor = std::function<std::shared_ptr<NodeClient>(const std::string& node_id)>;
    PeerTeam(std::shared_ptr<net::PeerTable> peers, ClientFor client_for, std::shared_ptr<store::CasStore> cas,
             std::string self_id);

    std::vector<std::pair<std::string, v1::CacheLookupReply>> Lookup(const v1::CacheLookup& req) override;
    std::optional<v1::CacheLookupReply> LookupAt(const std::string& node, const v1::CacheLookup& req) override;
    bool Pull(const std::string& node, const std::vector<v1::Digest>& blobs, const std::vector<v1::Digest>& trees) override;
    int Forget(const v1::CacheForget& req) override;

private:
    std::vector<std::string> Peers() const;

    std::shared_ptr<net::PeerTable> peers_;
    ClientFor client_for_;
    std::shared_ptr<store::CasStore> cas_;
    std::string self_id_;
};

} // namespace zeromesh::node
