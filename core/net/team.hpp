#pragma once

// Team secret (spec §9): 128 random bits shared out of band as a phrase.
// The team id is a public, one-way tag of the secret; Mac() authenticates
// team-scoped messages (discovery, admission) with domain-separating labels.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace zeromesh::net {

struct TeamKey {
    std::array<uint8_t, 16> secret;
};

TeamKey GenerateTeamKey(); // randombytes_buf

// "ZM-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXC": the secret as a 128-bit big-endian
// integer in Crockford base32 (26 symbols, the first one 0-7), then one Crockford
// mod-37 check symbol, grouped in fours.
std::string FormatPhrase(const TeamKey&);

// Case-insensitive; ignores '-' and whitespace; the "ZM" prefix is optional;
// I/L read as 1 and O as 0. nullopt on bad length, symbol or checksum.
std::optional<TeamKey> ParsePhrase(std::string_view);

// Lowercase hex of the first 16 bytes of SHA-256("zm-team-id" || secret).
std::string TeamId(const TeamKey&);

// HMAC-SHA256 with key = secret over label || 0x00 || data.
std::array<uint8_t, 32> Mac(const TeamKey&, std::string_view label, std::string_view data);

// Constant-time comparison (sodium_memcmp).
bool MacEqual(const std::array<uint8_t, 32>&, const std::array<uint8_t, 32>&);

// The joined team's secret, stored as its phrase in <state_dir>/team (mode 0600).
struct TeamFile {
    // nullopt when the file is missing or does not hold a valid phrase.
    static std::optional<TeamKey> Load(const std::filesystem::path& state_dir);
    // Creates state_dir if needed and replaces the file atomically. Throws std::system_error.
    static void Save(const std::filesystem::path& state_dir, const TeamKey&);
    static void Remove(const std::filesystem::path& state_dir);
};

} // namespace zeromesh::net
