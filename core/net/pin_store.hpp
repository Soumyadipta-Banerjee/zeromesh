#pragma once

// Pinned peers (spec §9): node id <-> certificate fingerprint, recorded after a
// successful admission. File <state_dir>/peers (mode 0600), one
// "node_id fingerprint" per line. Thread-safe; every change is written through.

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace zeromesh::net {

class PinStore {
public:
    // Loads <state_dir>/peers if present; malformed lines are skipped.
    explicit PinStore(std::filesystem::path state_dir);

    bool IsPinned(std::string_view fp) const;
    std::optional<std::string> NodeFor(std::string_view fp) const;
    std::optional<std::string> FingerprintFor(std::string_view node_id) const;

    // Pins (node_id, fp), replacing any older pin for node_id and any other node
    // pinned to fp. Throws std::invalid_argument on a malformed node id or
    // fingerprint and std::system_error if the file cannot be written.
    void Pin(std::string node_id, std::string fp);

    // Forgets every peer (leaving a team) and removes the file.
    void Clear();

private:
    void SaveLocked() const;

    std::filesystem::path path_;
    mutable std::mutex mu_;
    std::map<std::string, std::string, std::less<>> fp_by_node_;
};

} // namespace zeromesh::net
