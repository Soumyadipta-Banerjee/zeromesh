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

// Task table for ListActivity (submitted + executing; WORKING and DONE stay listed).
void RenderActivity(const v1::ActivitySnapshot& activity, std::ostream& os, bool color);

// Full monitor frame: peers + activity.
void RenderMonitor(const v1::PeerList& peers, const v1::ActivitySnapshot& activity, std::ostream& os, bool color);

using PeerFetcher = std::function<std::optional<v1::PeerList>(std::string* error, bool* unavailable)>;
using ActivityFetcher =
    std::function<std::optional<v1::ActivitySnapshot>(std::string* error, bool* unavailable)>;

// Full-screen monitor: redraws once a second until `stop` is set (Ctrl-C), 'q' or ESC on
// stdin, or the daemon goes away. Returns 0, or 2 when the daemon became unreachable.
int RunMonitor(const PeerFetcher& fetch_peers, const ActivityFetcher& fetch_activity,
               const std::atomic<bool>& stop);

} // namespace zeromesh::cli
