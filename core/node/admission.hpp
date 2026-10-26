#pragma once

// Admission (spec §9): a challenge-response keyed by the team secret, bound to
// both TLS certificates, after which each side pins the other's fingerprint.
//
//   client -> Admit{client_node_id, nonce_c}                     (32-byte nonces)
//   server -> {server_node_id, nonce_s, mac_s}
//             mac_s = Mac(key, "zm-admit-s", nonce_c || fp_server || fp_client)
//   client    verifies mac_s with the fp_server its TLS verifier saw
//   client -> AdmitConfirm{mac_c}
//             mac_c = Mac(key, "zm-admit-c", nonce_s || fp_client || fp_server)
//   server    verifies mac_c, pins (client_node_id, fp_client); client pins (server_node_id, fp_server)
//
// Fingerprints are fixed-length (64 hex chars) and nonces fixed-length, so the
// concatenations are unambiguous. The server keeps a pending admission per
// fp_client for 30 s; AdmitConfirm consumes it whether or not the MAC matches.

#include "net/team.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace zeromesh::node {

inline constexpr size_t kAdmitNonceBytes = 32;
inline constexpr std::chrono::seconds kAdmitPendingTtl{30};
inline constexpr size_t kAdmitMaxPending = 256;

std::string AdmitNonce(); // kAdmitNonceBytes random bytes (libsodium)

std::array<uint8_t, 32> AdmitServerMac(const net::TeamKey&, std::string_view nonce_c, std::string_view fp_server,
                                       std::string_view fp_client);
std::array<uint8_t, 32> AdmitClientMac(const net::TeamKey&, std::string_view nonce_s, std::string_view fp_client,
                                       std::string_view fp_server);

// Constant-time: true iff `received` is exactly 32 bytes equal to `expected`.
bool AdmitMacMatches(std::string_view received, const std::array<uint8_t, 32>& expected);

// Server-side pending admissions, keyed by the client's certificate fingerprint.
class PendingAdmissions {
public:
    struct Entry {
        std::string node_id;
        std::string nonce_s;
        std::chrono::steady_clock::time_point expires;
    };

    // Records (replacing any earlier entry for fp_client). Prunes expired entries and,
    // if still full, evicts the one closest to expiry.
    void Begin(const std::string& fp_client, std::string node_id, std::string nonce_s);

    // Removes and returns the entry for fp_client unless it is missing or expired.
    std::optional<Entry> Take(const std::string& fp_client);

private:
    std::mutex mu_;
    std::map<std::string, Entry> pending_;
};

} // namespace zeromesh::node
