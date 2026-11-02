#pragma once

// Cache keys of items.
//
// The command key ("weak") covers everything about an item except the content of its inputs: argv,
// environment variables, cwd, declared outputs, depfile and search directories, the environment's
// identity, the architecture and network access. Where outputs are written on the requester does
// not count.
//
// The full key adds the inputs, in one of three ways:
//   - Whole: the item reads the whole workspace (plus the outputs of the items it comes after):
//     the input tree's root digest.
//   - Inputs: the item declared which workspace paths it reads: the entries at those paths.
//   - Depfile: the item writes a dependency file. Its footprint (the files it named, and the name
//     listings of the item's search directories) is recorded after a successful run; a later run
//     computes the full key from each recorded footprint against its own inputs (two-phase
//     lookup). The listings catch a new file that would shadow one the compiler used before.
//
// Every successful run also records what it read under the command key, so a miss can be
// explained: the paths that changed since then.

#include "store/merkle.hpp"
#include "zeromesh/v1/node.pb.h"
#include "zeromesh/v1/run.pb.h"

#include <string>
#include <vector>

namespace zeromesh::run {

enum class KeyMode { Whole, Inputs, Depfile };

KeyMode ModeOf(const v1::Item& item);

std::string WeakKey(const v1::Item& item, const v1::Stage& stage, const std::string& env_identity,
                    const std::string& arch);

// `path` joined onto `cwd` (both workspace-relative), normalized; "" for the root. Throws
// std::invalid_argument for a path that leaves the workspace.
std::string WorkspacePath(const std::string& cwd, const std::string& path);

// What an item reads, with the values those paths have in `root`. For Whole: one entry ("", root).
// For Inputs: the declared paths. For Depfile: `footprint` (workspace-relative files the depfile
// named) and the item's search directories as listings.
v1::Footprint Observe(store::MerkleBuilder& merkle, const store::Digest& root, const v1::Item& item,
                      const std::vector<std::string>& footprint = {});

// The full key for `weak` given what was observed.
std::string FullKey(const std::string& weak, const v1::Footprint& observed);

// The same paths as `recorded`, valued in `root` (to compute the key a recorded footprint gives now).
v1::Footprint Revalue(store::MerkleBuilder& merkle, const store::Digest& root, const v1::Footprint& recorded);

// Paths whose values differ between an earlier record and now (at most `limit`).
std::vector<std::string> Changed(store::MerkleBuilder& merkle, const v1::Footprint& before, const v1::Footprint& now,
                                 size_t limit);

} // namespace zeromesh::run
