#pragma once

// env-host: runs batches against the executing node's own installed software. The donor's /usr,
// /opt, its usr-merge layout and /etc are mounted read-only.

#include "plugin/plugin.hpp"

#include <memory>
#include <optional>
#include <string>

namespace zeromesh::envhost {

// PATH inside a host sandbox.
std::string DefaultPathEnv();

// Where `program` (a bare name on DefaultPathEnv(), or an absolute path under a mounted
// directory) is on this node; nullopt when a host sandbox could not run it.
std::optional<std::string> Resolve(const std::string& program);

std::unique_ptr<plugin::IEnvProvider> Make();

} // namespace zeromesh::envhost
