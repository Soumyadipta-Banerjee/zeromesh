#pragma once

#include "zeromesh/v1/client.pb.h"

#include <atomic>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace zeromesh::cli {

// Prints this node (the peer with is_self) and the other peers from a ListPeers reply.
// `color` adds ANSI colours (use it when writing to a terminal).
void RenderStatus(const v1::PeerList& peers, std::ostream& os, bool color);

// Returns the current peer list, or nullopt with *error set (and *unavailable when the
// daemon went away).
using PeerFetcher = std::function<std::optional<v1::PeerList>(std::string* error, bool* unavailable)>;

// Full-screen monitor: redraws once a second until `stop` is set (Ctrl-C), 'q' or ESC on
// stdin, or the daemon goes away. Returns 0, or 2 when the daemon became unreachable.
int RunMonitor(const PeerFetcher& fetch, const std::atomic<bool>& stop);

} // namespace zeromesh::cli
