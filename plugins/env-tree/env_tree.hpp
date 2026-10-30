#pragma once

// env-tree: runs a batch against a captured environment, a content-addressed tree of absolute
// paths built on the requester (`zm env capture`). Where /dev/fuse is available the tree is mounted
// lazily from the requester's store (only what the batch reads moves, in recorded order on later
// runs); otherwise it is fetched whole once into the node's cache and reused.

#include "plugin/plugin.hpp"

#include <memory>

namespace zeromesh::tree {

// `allow_fuse` false always fetches whole (what a node without /dev/fuse does).
std::unique_ptr<plugin::IEnvProvider> MakeTreeEnv(bool allow_fuse = true);

} // namespace zeromesh::tree
