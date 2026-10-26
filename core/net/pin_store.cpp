#include "net/pin_store.hpp"

#include "net/identity.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace zeromesh::net {

PinStore::PinStore(std::filesystem::path state_dir) : path_(std::move(state_dir) / "peers") {
    std::ifstream in(path_);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string node, fp, extra;
        if (!(ss >> node >> fp) || (ss >> extra)) continue;
        if (!IsValidNodeId(node) || !IsFingerprint(fp)) continue;
        fp_by_node_[node] = fp;
    }
}

bool PinStore::IsPinned(std::string_view fp) const { return NodeFor(fp).has_value(); }

std::optional<std::string> PinStore::NodeFor(std::string_view fp) const {
    if (!IsFingerprint(fp)) return std::nullopt;
    std::lock_guard lock(mu_);
    for (const auto& [node, pinned] : fp_by_node_) {
        if (pinned == fp) return node;
    }
    return std::nullopt;
}

std::optional<std::string> PinStore::FingerprintFor(std::string_view node_id) const {
    std::lock_guard lock(mu_);
    auto it = fp_by_node_.find(node_id);
    if (it == fp_by_node_.end()) return std::nullopt;
    return it->second;
}

size_t PinStore::Count() const {
    std::lock_guard lock(mu_);
    return fp_by_node_.size();
}

void PinStore::Pin(std::string node_id, std::string fp) {
    if (!IsValidNodeId(node_id)) throw std::invalid_argument("pin: invalid node id");
    if (!IsFingerprint(fp)) throw std::invalid_argument("pin: invalid fingerprint");
    std::lock_guard lock(mu_);
    auto before = fp_by_node_;
    for (auto it = fp_by_node_.begin(); it != fp_by_node_.end();) {
        it = (it->second == fp && it->first != node_id) ? fp_by_node_.erase(it) : std::next(it);
    }
    fp_by_node_[std::move(node_id)] = std::move(fp);
    try {
        SaveLocked();
    } catch (...) {
        fp_by_node_ = std::move(before); // memory never claims a pin the file lacks
        throw;
    }
}

void PinStore::Clear() {
    std::lock_guard lock(mu_);
    fp_by_node_.clear();
    std::error_code ec;
    std::filesystem::remove(path_, ec);
}

void PinStore::SaveLocked() const {
    std::filesystem::create_directories(path_.parent_path());
    std::string body;
    for (const auto& [node, fp] : fp_by_node_) body += node + " " + fp + "\n";
    WritePrivateFileAtomic(path_, body);
}

} // namespace zeromesh::net
