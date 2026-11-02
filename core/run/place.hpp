#pragma once

// Writes item outputs from the store to their destinations on the requester. Every file gets its
// own copy (never a link into the store), written beside the target and renamed over it, so a
// reader sees the old file or the new one, never half of one. A file that already has the right
// content and mode is left alone.

#include "store/merkle.hpp"
#include "store/store.hpp"
#include "zeromesh/v1/run.pb.h"

#include <filesystem>
#include <string>

namespace zeromesh::run {

// Writes `out` at `dest` (absolute; parents are created). False with *error on failure.
bool Place(store::CasStore& cas, store::MerkleBuilder& merkle, const v1::OutputFile& out,
           const std::filesystem::path& dest, std::string* error);

// Writes `out` at root/out.path(). The relative path comes from a donor, so it must stay below
// `root` and may not pass through a symlink there.
bool PlaceUnder(store::CasStore& cas, store::MerkleBuilder& merkle, const v1::OutputFile& out,
                const std::filesystem::path& root, std::string* error, std::filesystem::path* placed);

} // namespace zeromesh::run
