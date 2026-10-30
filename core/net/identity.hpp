#pragma once

// Node identity (spec §9): each node owns an ECDSA P-256 key and a self-signed
// X.509 certificate whose CN is the node id. Peers know each other by the
// certificate fingerprint, pinned during admission; the certificate chain is
// never verified against a CA.

#include <filesystem>
#include <string>
#include <string_view>

namespace zeromesh::net {

struct Identity {
    std::string node_id; // the certificate's CN
    std::string cert_pem;
    std::string key_pem;     // PKCS#8, unencrypted; stored mode 0600
    std::string fingerprint; // lowercase hex SHA-256 of the DER certificate (64 chars)

    // Loads <state_dir>/node.crt + node.key, or creates them (mode 0600) when either
    // is missing or the certificate's CN is not `node_id` (the node id changed, so the
    // node is a new node). Creates state_dir if needed. Throws std::runtime_error on
    // I/O failure or when existing files are malformed or do not belong together.
    static Identity LoadOrCreate(const std::filesystem::path& state_dir, std::string_view node_id);
};

// Fingerprint of the first certificate in `pem`, or "" if it does not parse.
std::string FingerprintOfPem(std::string_view pem);

// True for a 64-char lowercase hex string.
bool IsFingerprint(std::string_view fp);

// Node ids go into the pin file and the certificate CN: 1-64 chars of [A-Za-z0-9._-].
bool IsValidNodeId(std::string_view node_id);

// Writes `content` to `path` with mode 0600 via a temp file + fsync + rename.
// Throws std::system_error.
void WritePrivateFileAtomic(const std::filesystem::path& path, std::string_view content);

} // namespace zeromesh::net
