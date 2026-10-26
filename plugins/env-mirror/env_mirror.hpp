#pragma once

#include "plugin/plugin.hpp"
#include "plugins/env-mirror/chunk_source.hpp"
#include "plugins/env-mirror/mirror_fuse.hpp"

#include <memory>
#include <string>

namespace zeromesh::mirror {

/**
 * @brief Factory function creating an env-mirror IEnvProvider instance.
 */
std::unique_ptr<plugin::IEnvProvider> MakeMirrorEnv();

} // namespace zeromesh::mirror
