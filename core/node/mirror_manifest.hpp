#pragma once

#include "store/merkle.hpp"
#include "store/store.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace zeromesh::node {

inline constexpr const char* kMirrorPathsParam = "mirror_paths";
inline constexpr const char* kMirrorRootDigestParam = "root_digest";
inline constexpr const char* kMirrorRequesterParam = "requester_node_id";

// Parses mirror_paths (newline-separated absolute paths) from EnvSpec params.
std::vector<std::filesystem::path> ParseMirrorPaths(const std::map<std::string, std::string>& params);

// Builds a Merkle root for the paths, applying the mirror deny list. Paths must exist.
store::Digest BuildMirrorManifest(store::MerkleBuilder& merkle, const std::vector<std::filesystem::path>& paths,
                                  uint64_t* total_bytes = nullptr);

} // namespace zeromesh::node
