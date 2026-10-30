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

// One row per task (WORKING and DONE stay listed). `peers` maps executed_by ids to hostnames.
void RenderActivity(const v1::ActivitySnapshot& activity, std::ostream& os, bool color,
                    const v1::PeerList* peers = nullptr);

// Compact monitor frame: this node, peers, tasks. Fits a tiled pane.
// tab: 0 = Live (queued/placed/running), 1 = Done (succeeded/failed/cancelled).
// scroll: first visible task index within the active tab; clamped by the renderer.
void RenderMonitor(const v1::PeerList& peers, const v1::ActivitySnapshot& activity, std::ostream& os, bool color,
                   int tab = 0, int scroll = 0, int rows = 0, int cols = 0);

using PeerFetcher = std::function<std::optional<v1::PeerList>(std::string* error, bool* unavailable)>;
using ActivityFetcher = std::function<std::optional<v1::ActivitySnapshot>(std::string* error, bool* unavailable)>;
// Returns false and sets `error` when the daemon could not clear its task history.
using ActivityClearer = std::function<bool(std::string* error)>;

// Full-screen monitor: redraws once a second until `stop` is set (Ctrl-C), 'q' or ESC on
// stdin, or the daemon goes away. Returns 0, or 2 when the daemon became unreachable.
// Keys: 1/2 or Tab switch Live/Done; j/k / arrows / PgUp/PgDn / mouse wheel scroll;
// c clears finished tasks (running ones stay).
int RunMonitor(const PeerFetcher& fetch_peers, const ActivityFetcher& fetch_activity,
               const ActivityClearer& clear_activity, const std::atomic<bool>& stop);

} // namespace zeromesh::cli
