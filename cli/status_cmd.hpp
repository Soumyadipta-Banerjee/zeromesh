#pragma once

#include "cli/client.hpp"

namespace zeromesh::cli {

// zm status: print this node and the peer table once.
int StatusCommand(Client& client);

// zm monitor: live peer view until Ctrl-C or q.
int MonitorCommand(Client& client);

} // namespace zeromesh::cli
