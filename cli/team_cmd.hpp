#pragma once

#include "cli/client.hpp"

#include <string>
#include <vector>

namespace zeromesh::cli {

// zm team create | join <phrase> | leave | info
int TeamCommand(Client& client, const std::vector<std::string>& args);

} // namespace zeromesh::cli
