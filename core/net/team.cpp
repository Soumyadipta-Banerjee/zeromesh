#include "net/team.hpp"

#include <fcntl.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>

namespace zeromesh::net {

namespace {

namespace fs = std::filesystem;

// Crockford base32 symbols; indices 32-36 are valid only as the check symbol.
constexpr std::string_view kSymbols = "0123456789ABCDEFGHJKMNPQRSTVWXYZ*~$=U";
constexpr size_t kDataSymbols = 26; // ceil(128 / 5)
constexpr size_t kGroup = 4;

void EnsureSodium() {
    static const int rc = sodium_init(); // 0 first time, 1 if already initialised
    if (rc < 0) throw std::runtime_error("sodium_init failed");
}

// Symbol value for an upper-cased character, or -1.
int SymbolValue(char c) {
    if (c == 'I' || c == 'L') return 1;
    if (c == 'O') return 0;
    size_t pos = kSymbols.find(c);
    return pos == std::string_view::npos ? -1 : static_cast<int>(pos);
}

unsigned Mod37(const std::array<uint8_t, 16>& v) {
    unsigned r = 0;
    for (uint8_t b : v) r = (r * 256 + b) % 37;
    return r;
}

fs::path TeamPath(const fs::path& state_dir) { return state_dir / "team"; }

[[noreturn]] void ThrowErrno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

TeamKey GenerateTeamKey() {
    EnsureSodium();
    TeamKey key{};
    randombytes_buf(key.secret.data(), key.secret.size());
    return key;
}

std::string FormatPhrase(const TeamKey& key) {
    // 130 bits of symbols hold the 128-bit value with two leading zero bits.
    std::string sym(kDataSymbols, '0');
    unsigned acc = 0;
    int bits = 0;
    size_t out = kDataSymbols;
    for (auto it = key.secret.rbegin(); it != key.secret.rend(); ++it) {
        acc |= static_cast<unsigned>(*it) << bits;
        bits += 8;
        while (bits >= 5 && out > 0) {
            sym[--out] = kSymbols[acc & 31];
            acc >>= 5;
            bits -= 5;
        }
    }
    if (out > 0) sym[--out] = kSymbols[acc & 31];
    sym.push_back(kSymbols[Mod37(key.secret)]);

    std::string phrase = "ZM";
    for (size_t i = 0; i < sym.size(); ++i) {
        if (i % kGroup == 0) phrase.push_back('-');
        phrase.push_back(sym[i]);
    }
    return phrase;
}

std::optional<TeamKey> ParsePhrase(std::string_view text) {
    std::string s;
    for (char c : text) {
        if (c == '-' || std::isspace(static_cast<unsigned char>(c))) continue;
        s.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    if (s.size() == kDataSymbols + 3 && s.starts_with("ZM")) s.erase(0, 2);
    if (s.size() != kDataSymbols + 1) return std::nullopt;

    // Big-endian accumulate 26 x 5 bits into 16 bytes; the top two bits must be zero.
    TeamKey key{};
    for (size_t i = 0; i < kDataSymbols; ++i) {
        int v = SymbolValue(s[i]);
        if (v < 0 || v >= 32) return std::nullopt;
        if (i == 0 && v >= 8) return std::nullopt;
        unsigned carry = static_cast<unsigned>(v);
        for (auto it = key.secret.rbegin(); it != key.secret.rend(); ++it) {
            unsigned x = (static_cast<unsigned>(*it) << 5) | carry;
            *it = static_cast<uint8_t>(x & 0xff);
            carry = x >> 8;
        }
    }
    int check = SymbolValue(s.back());
    if (check < 0 || static_cast<unsigned>(check) != Mod37(key.secret)) return std::nullopt;
    return key;
}

std::string TeamId(const TeamKey& key) {
    EnsureSodium();
    static constexpr std::string_view kLabel = "zm-team-id";
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, reinterpret_cast<const unsigned char*>(kLabel.data()), kLabel.size());
    crypto_hash_sha256_update(&st, key.secret.data(), key.secret.size());
    unsigned char digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&st, digest);
    char hex[16 * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex), digest, 16);
    return hex;
}

std::array<uint8_t, 32> Mac(const TeamKey& key, std::string_view label, std::string_view data) {
    EnsureSodium();
    static_assert(crypto_auth_hmacsha256_BYTES == 32);
    crypto_auth_hmacsha256_state st;
    crypto_auth_hmacsha256_init(&st, key.secret.data(), key.secret.size());
    crypto_auth_hmacsha256_update(&st, reinterpret_cast<const unsigned char*>(label.data()), label.size());
    const unsigned char sep = 0;
    crypto_auth_hmacsha256_update(&st, &sep, 1);
    crypto_auth_hmacsha256_update(&st, reinterpret_cast<const unsigned char*>(data.data()), data.size());
    std::array<uint8_t, 32> out{};
    crypto_auth_hmacsha256_final(&st, out.data());
    sodium_memzero(&st, sizeof(st));
    return out;
}

bool MacEqual(const std::array<uint8_t, 32>& a, const std::array<uint8_t, 32>& b) {
    EnsureSodium();
    return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

std::optional<TeamKey> TeamFile::Load(const fs::path& state_dir) {
    std::ifstream in(TeamPath(state_dir));
    if (!in) return std::nullopt;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ParsePhrase(text);
}

void TeamFile::Save(const fs::path& state_dir, const TeamKey& key) {
    fs::create_directories(state_dir);
    const fs::path final_path = TeamPath(state_dir);
    const fs::path tmp = state_dir / ".team.tmp";
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) ThrowErrno("create " + tmp.string());
    const std::string body = FormatPhrase(key) + "\n";
    bool ok = ::fchmod(fd, 0600) == 0 &&
              ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()) &&
              ::fsync(fd) == 0;
    int saved = errno;
    ::close(fd);
    if (!ok || ::rename(tmp.c_str(), final_path.c_str()) != 0) {
        if (ok) saved = errno;
        ::unlink(tmp.c_str());
        errno = saved;
        ThrowErrno("write " + final_path.string());
    }
}

void TeamFile::Remove(const fs::path& state_dir) {
    std::error_code ec;
    fs::remove(TeamPath(state_dir), ec);
}

} // namespace zeromesh::net
