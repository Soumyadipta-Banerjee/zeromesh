#pragma once

// env-host: runs tasks against the executing node's own installed software
// (spec §7.1). The donor's /usr, its usr-merge layout and a few /etc files are
// mounted read-only; Check() only accepts commands that resolve on this node.

#include "plugin/plugin.hpp"

#include <memory>
#include <string>

namespace zeromesh::envhost {

// PATH exposed inside the host sandbox (same string Check() resolves against by default).
std::string DefaultPathEnv();

// `path_env` is the ':'-separated search path Check() resolves argv[0] against.
std::unique_ptr<plugin::IEnvProvider> Make(std::string path_env = DefaultPathEnv());

} // namespace zeromesh::envhost
