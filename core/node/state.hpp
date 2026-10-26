#pragma once

#include <filesystem>
#include <string>

namespace zeromesh::node {

// Persistent per-node state rooted at a state directory (default ~/.local/share/zeromesh).
// The node id is 32 lowercase hex chars (128 random bits), generated once and kept in
// <dir>/node_id with mode 0600.
class NodeState {
public:
    // Creates `state_dir` (mode 0700) if missing, then loads the node id or creates it.
    // Throws std::runtime_error on I/O failure or if an existing node_id file is malformed.
    static NodeState LoadOrCreate(const std::filesystem::path& state_dir);

    const std::string& node_id() const { return node_id_; }
    const std::filesystem::path& dir() const { return dir_; }

private:
    NodeState(std::filesystem::path dir, std::string node_id)
        : dir_(std::move(dir)), node_id_(std::move(node_id)) {}

    std::filesystem::path dir_;
    std::string node_id_;
};

} // namespace zeromesh::node
