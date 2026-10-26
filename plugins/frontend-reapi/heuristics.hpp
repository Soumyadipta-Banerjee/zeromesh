#pragma once

#include <string>
#include <vector>

namespace zeromesh::frontend_reapi {

/**
 * @brief Locality heuristics for build commands.
 *
 * Linkers and fat archives are pinned locally (may_run_remote = false)
 * to prevent heavy binary transfer overhead on serial bottlenecks.
 */
bool IsCriticalPath(const std::vector<std::string>& command_args);

} // namespace zeromesh::frontend_reapi
