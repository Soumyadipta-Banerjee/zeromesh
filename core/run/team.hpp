#pragma once

// How the run engine reaches the rest of the team for the item cache. The daemon implements it
// over the admitted active peers; tests supply fakes.

#include "zeromesh/v1/common.pb.h"
#include "zeromesh/v1/node.pb.h"

#include <string>
#include <utility>
#include <vector>

namespace zeromesh::run {

class Team {
public:
    virtual ~Team() = default;
    // Asks every reachable peer at once; returns (node id, reply) for each that answered.
    virtual std::vector<std::pair<std::string, v1::CacheLookupReply>> Lookup(const v1::CacheLookup& req) = 0;
    // Asks one peer.
    virtual std::optional<v1::CacheLookupReply> LookupAt(const std::string& node, const v1::CacheLookup& req) = 0;
    // Fetches blobs, and trees with everything under them, from `node` into this node's store.
    virtual bool Pull(const std::string& node, const std::vector<v1::Digest>& blobs,
                      const std::vector<v1::Digest>& trees) = 0;
    // Tells every reachable peer to forget entries. Returns how many peers were told.
    virtual int Forget(const v1::CacheForget& req) = 0;
};

} // namespace zeromesh::run
