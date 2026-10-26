#include "node/state.hpp"

#include <fcntl.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace zeromesh::node {

namespace {

namespace fs = std::filesystem;

constexpr size_t kIdBytes = 16;

[[noreturn]] void Fail(const std::string& what, const fs::path& p) {
    throw std::runtime_error(what + " " + p.string() + ": " + std::strerror(errno));
}

bool IsValidId(const std::string& id) {
    if (id.size() != kIdBytes * 2) return false;
    for (char c : id) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string GenerateId() {
    if (sodium_init() < 0) throw std::runtime_error("sodium_init failed");
    unsigned char raw[kIdBytes];
    randombytes_buf(raw, sizeof(raw));
    static constexpr char kHex[] = "0123456789abcdef";
    std::string id;
    id.reserve(kIdBytes * 2);
    for (unsigned char b : raw) {
        id.push_back(kHex[b >> 4]);
        id.push_back(kHex[b & 0x0f]);
    }
    return id;
}

// Writes `content` to `path` with mode 0600 via a temp file + rename, so a crash never
// leaves a truncated id behind.
void WriteFileAtomic0600(const fs::path& path, const std::string& content) {
    const fs::path tmp = path.string() + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) Fail("cannot create", tmp);
    if (::fchmod(fd, 0600) != 0) {
        ::close(fd);
        Fail("cannot chmod", tmp);
    }
    size_t off = 0;
    while (off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            Fail("cannot write", tmp);
        }
        off += static_cast<size_t>(n);
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) Fail("cannot flush", tmp);
    if (::rename(tmp.c_str(), path.c_str()) != 0) Fail("cannot rename to", path);
}

}  // namespace

NodeState NodeState::LoadOrCreate(const fs::path& state_dir) {
    std::error_code ec;
    if (!fs::exists(state_dir, ec)) {
        fs::create_directories(state_dir, ec);
        if (ec) throw std::runtime_error("cannot create state dir " + state_dir.string() + ": " + ec.message());
        if (::chmod(state_dir.c_str(), 0700) != 0) Fail("cannot chmod", state_dir);
    } else if (!fs::is_directory(state_dir, ec)) {
        throw std::runtime_error("state dir is not a directory: " + state_dir.string());
    }

    const fs::path id_path = state_dir / "node_id";
    if (fs::exists(id_path, ec)) {
        std::ifstream in(id_path);
        std::string id;
        std::getline(in, id);
        if (!IsValidId(id)) {
            throw std::runtime_error("malformed node id in " + id_path.string() +
                                     " (expected 32 lowercase hex chars)");
        }
        return NodeState(state_dir, id);
    }

    std::string id = GenerateId();
    WriteFileAtomic0600(id_path, id + "\n");
    return NodeState(state_dir, std::move(id));
}

}  // namespace zeromesh::node
