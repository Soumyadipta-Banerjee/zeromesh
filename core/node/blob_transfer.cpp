#include "node/blob_transfer.hpp"

#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <system_error>

namespace zeromesh::node {

namespace {

std::string Hex(const unsigned char* md, unsigned int len) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out.push_back(kDigits[md[i] >> 4]);
        out.push_back(kDigits[md[i] & 0xf]);
    }
    return out;
}

} // namespace

// One object being received: a private temp file next to the blob store plus a running hash.
struct BlobReceiver::Object {
    store::Digest declared;
    std::filesystem::path tmp;
    int fd{-1};
    int64_t received{0};
    EVP_MD_CTX* md{nullptr};

    ~Object() {
        if (md) EVP_MD_CTX_free(md);
        if (fd >= 0) ::close(fd);
        if (!tmp.empty()) {
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
        }
    }
};

BlobReceiver::BlobReceiver(store::CasStore& cas) : cas_(cas) {}

BlobReceiver::~BlobReceiver() = default;

void BlobReceiver::Abort() { current_.reset(); }

std::string BlobReceiver::Feed(const v1::Chunk& chunk) {
    if (!current_) {
        if (!chunk.has_digest()) return Abort(), "chunk without a digest outside an object";
        const v1::Digest& d = chunk.digest();
        if (!store::CasStore::IsValidHash(d.hash())) return Abort(), "invalid digest hash";
        if (d.size_bytes() < 0) return Abort(), "invalid digest size";

        auto obj = std::make_unique<Object>();
        obj->declared = store::Digest{d.hash(), d.size_bytes()};
        std::error_code ec;
        const auto dir = cas_.GetBasePath() / "incoming";
        std::filesystem::create_directories(dir, ec);
        std::string tmpl = (dir / "push-XXXXXX").string();
        obj->fd = ::mkstemp(tmpl.data());
        if (obj->fd < 0) return std::string("cannot create temp file: ") + std::strerror(errno);
        obj->tmp = tmpl;
        ::fchmod(obj->fd, 0644);
        obj->md = EVP_MD_CTX_new();
        if (!obj->md || EVP_DigestInit_ex(obj->md, EVP_sha256(), nullptr) != 1) return Abort(), "sha256 init failed";
        current_ = std::move(obj);
    } else if (chunk.has_digest()) {
        return Abort(), "a new object started before the previous one ended";
    }

    Object& obj = *current_;
    const std::string& data = chunk.data();
    if (static_cast<int64_t>(data.size()) > obj.declared.size_bytes - obj.received) {
        std::string err = "object " + obj.declared.hash + " is longer than its declared size";
        Abort();
        return err;
    }
    if (!data.empty()) {
        if (EVP_DigestUpdate(obj.md, data.data(), data.size()) != 1) return Abort(), "sha256 update failed";
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::write(obj.fd, data.data() + off, data.size() - off);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                std::string err = std::string("cannot write blob: ") + std::strerror(errno);
                Abort();
                return err;
            }
            off += static_cast<size_t>(n);
        }
        obj.received += static_cast<int64_t>(data.size());
    }
    if (!chunk.last()) return {};

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(obj.md, md, &len) != 1) return Abort(), "sha256 final failed";
    const std::string actual = Hex(md, len);
    if (actual != obj.declared.hash || obj.received != obj.declared.size_bytes) {
        std::string err = "digest mismatch: declared " + obj.declared.hash + "/" +
                          std::to_string(obj.declared.size_bytes) + ", received " + actual + "/" +
                          std::to_string(obj.received);
        Abort();
        return err;
    }
    // No fsync, like CasStore::PutBlob: the blob is published by an atomic rename.
    const bool closed = ::close(obj.fd) == 0;
    obj.fd = -1;
    if (!closed) {
        std::string err = std::string("cannot write blob: ") + std::strerror(errno);
        Abort();
        return err;
    }
    if (!cas_.HasBlob(actual)) {
        std::error_code ec;
        std::filesystem::rename(obj.tmp, cas_.GetBlobPath(actual), ec);
        if (ec) return Abort(), "cannot store blob: " + ec.message();
        obj.tmp.clear();
    }
    received_.push_back(obj.declared);
    current_.reset();
    return {};
}

std::string BlobReceiver::Finish() const {
    if (current_) return "stream ended inside object " + current_->declared.hash;
    return {};
}

std::string SendBlob(const store::CasStore& cas, const store::Digest& d,
                     const std::function<bool(const v1::Chunk&)>& write) {
    if (!store::CasStore::IsValidHash(d.hash)) return "invalid digest hash";
    const auto path = cas.GetBlobPath(d.hash);
    std::ifstream in(path, std::ios::binary);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (!in || ec) return "blob " + d.hash + " not found";

    v1::Chunk chunk;
    chunk.mutable_digest()->set_hash(d.hash);
    chunk.mutable_digest()->set_size_bytes(static_cast<int64_t>(size));
    std::string buf(kBlobChunkBytes, '\0');
    uint64_t sent = 0;
    do {
        size_t want = static_cast<size_t>(std::min<uint64_t>(kBlobChunkBytes, size - sent));
        in.read(buf.data(), static_cast<std::streamsize>(want));
        if (static_cast<size_t>(in.gcount()) != want) return "short read of blob " + d.hash;
        sent += want;
        chunk.set_data(buf.data(), want);
        chunk.set_last(sent == size);
        if (!write(chunk)) return "transport closed while sending blob " + d.hash;
        chunk.clear_digest();
    } while (sent < size);
    return {};
}

} // namespace zeromesh::node
