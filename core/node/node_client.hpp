#pragma once

// NodeApi client, one per peer (spec §6.1, §9). Admit() runs the admission
// handshake over a channel that accepts whichever server certificate it sees
// first (and only that one for the rest of the handshake), then pins the peer.
// Every other call goes through a channel whose TLS verifier accepts only the
// fingerprint pinned for the peer's node id, with transport keepalive (pings every
// 5 s, dead after 10 s without an answer). Unary RPCs have a 10 s deadline; blob
// transfers get 10 s plus 1 s per MiB (120 s base when size is unknown); Run has no deadline and relies on keepalive.
// Thread-safe: one client may serve many concurrent calls.

#include "net/identity.hpp"
#include "net/pin_store.hpp"
#include "net/team.hpp"
#include "store/store.hpp"

#include "zeromesh/v1/node.grpc.pb.h"

#include <grpcpp/channel.h>
#include <grpcpp/server_builder.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace zeromesh::node {

inline constexpr std::chrono::seconds kNodeRpcTimeout{10};
// OfferTask carries every input-tree digest; large trees need a bigger gRPC frame than the default 4 MiB.
inline constexpr int kNodeApiMaxMessageSize = 64 * 1024 * 1024;

inline void ApplyNodeApiMessageLimits(grpc::ServerBuilder& builder) {
    builder.SetMaxReceiveMessageSize(kNodeApiMaxMessageSize);
    builder.SetMaxSendMessageSize(kNodeApiMaxMessageSize);
}

// Serialized size of an Offer RPC (for placer-side limits before calling the peer).
[[nodiscard]] size_t EncodedOfferByteSize(const std::string& task_id, const v1::TaskSpec& spec,
                                          const std::vector<v1::Digest>& needed);

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

    // Round-trip time of a Ping RPC (wall clock), or nullopt on failure.
    std::optional<std::chrono::milliseconds> PingRtt(std::string* error = nullptr);

    // Offers a task; `needed` lists every blob the run reads (the input tree's blobs).
    // nullopt (with *error) when the call itself failed.
    std::optional<v1::OfferReply> Offer(const std::string& task_id, const v1::TaskSpec& spec,
                                        const std::vector<v1::Digest>& needed, std::string* error = nullptr);

    // Uploads `digests` from `cas` to the peer, which verifies every object.
    bool Push(const store::CasStore& cas, const std::vector<v1::Digest>& digests, std::string* error = nullptr);

    // Downloads `digests` into `cas`. Every object is verified against its digest before it
    // is stored, and the peer may send only what was asked for.
    bool Pull(store::CasStore& cas, const std::vector<v1::Digest>& digests, std::string* error = nullptr);

    // Runs a task the peer accepted, calling on_event for every event, and blocks until the
    // final event, whose result it returns. nullopt (with *error) when the stream broke
    // first, e.g. the peer vanished (keepalive timeout) or refused the run. After
    // Cancel(task_id) it returns the peer's cancelled result, or a local one if the peer
    // could not be told.
    std::optional<v1::TaskResult> Run(const std::string& task_id,
                                      const std::function<void(const v1::JobEvent&)>& on_event,
                                      std::string* error = nullptr);

    // Cancels the task on the peer (CancelTask); if the peer cannot be reached, aborts
    // the local Run stream instead. A no-op for a task with no Run in progress.
    void Cancel(const std::string& task_id);

    // Tells the peer this node is draining.
    bool Drain(const std::string& reason, std::string* error = nullptr);

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
    std::map<std::string, grpc::ClientContext*> runs_; // task id -> its active Run call
    std::set<std::string> cancelled_;                  // tasks Cancel()ed during their Run
};

} // namespace zeromesh::node
