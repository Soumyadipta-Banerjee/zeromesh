#pragma once

#include "cli/client.hpp"

namespace zeromesh::cli {

// zm status: print this node and the peer table once.
int StatusCommand(Client& client);

// zm monitor: live peer + task view until Ctrl-C or q (1/2 tabs, j/k scroll).
int MonitorCommand(Client& client);

} // namespace zeromesh::cli
