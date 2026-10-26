#pragma once

// NodeApi client, one per peer (spec §6.1, §9). Admit() runs the admission
// handshake over a channel that accepts whichever server certificate it sees
// first (and only that one for the rest of the handshake), then pins the peer.
// Every other call goes through a channel whose TLS verifier accepts only the
// fingerprint pinned for the peer's node id. All RPCs have a 10 s deadline.

#include "net/identity.hpp"
#include "net/pin_store.hpp"
#include "net/team.hpp"

#include "zeromesh/v1/node.grpc.pb.h"

#include <grpcpp/channel.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace zeromesh::node {

inline constexpr std::chrono::seconds kNodeRpcTimeout{10};

class NodeClient {
public:
    // `address` is "ip:port". `expected_node_id`, when set, is the peer's node id as
    // learned from discovery: admission fails if the server claims another id, and
    // calls can use an existing pin without admitting first.
    NodeClient(std::string address, net::Identity self, std::shared_ptr<net::PinStore> pins,
               std::optional<std::string> expected_node_id = std::nullopt);

    // Runs admission with `key`. On success both sides have pinned each other.
    bool Admit(const net::TeamKey& key, std::string* error = nullptr);

    std::optional<v1::PeerInfo> Ping(std::string* error = nullptr);

    const std::string& address() const { return address_; }
    // The peer's node id once known (expected, or learned by Admit).
    std::optional<std::string> peer_node_id() const;

private:
    // A stub on the pinned channel, or nullptr (with *error set) if the peer's node
    // id is unknown or has no pin.
    std::shared_ptr<v1::NodeApi::Stub> PinnedStub(std::string* error);
    static void SetDeadline(grpc::ClientContext& ctx);

    const std::string address_;
    const net::Identity self_;
    const std::shared_ptr<net::PinStore> pins_;

    mutable std::mutex mu_;
    std::optional<std::string> peer_node_id_;
    std::shared_ptr<v1::NodeApi::Stub> pinned_stub_;
};

} // namespace zeromesh::node
