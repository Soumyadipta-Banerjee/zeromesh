#include "run/place.hpp"

#include <fcntl.h>
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <optional>
#include <vector>

namespace zeromesh::run {

namespace {

namespace fs = std::filesystem;

constexpr size_t kCopyChunk = 1 << 20;

std::string RandomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    randombytes_buf(raw.data(), raw.size());
    std::string out;
    for (unsigned char c : raw) {
        out.push_back("0123456789abcdef"[c >> 4]);
        out.push_back("0123456789abcdef"[c & 15]);
    }
    return out;
}

std::string Errno(const std::string& what, const fs::path& p) {
    return what + " " + p.string() + ": " + std::strerror(errno);
}

bool CopyFile(const fs::path& src, const fs::path& dst, bool executable, std::string* error) {
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        *error = Errno("cannot open blob", src);
        return false;
    }
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) {
        *error = Errno("cannot create", dst);
        close(in);
        return false;
    }
    std::vector<char> buf(kCopyChunk);
    bool ok = true;
    for (;;) {
        ssize_t n = read(in, buf.data(), buf.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            *error = Errno("cannot read blob", src);
            ok = false;
            break;
        }
        if (n == 0) break;
        for (ssize_t off = 0; off < n && ok;) {
            ssize_t w = write(out, buf.data() + off, static_cast<size_t>(n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) {
                *error = Errno("cannot write", dst);
                ok = false;
                break;
            }
            off += w;
        }
        if (!ok) break;
    }
    if (ok && fchmod(out, executable ? 0755 : 0644) != 0) {
        *error = Errno("cannot chmod", dst);
        ok = false;
    }
    close(in);
    if (close(out) != 0 && ok) {
        *error = Errno("cannot write", dst);
        ok = false;
    }
    return ok;
}

// True when `target` already is a regular file with exactly this content and mode.
bool AlreadyCurrent(const fs::path& target, const v1::OutputFile& out) {
    struct stat st{};
    if (lstat(target.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (static_cast<int64_t>(st.st_size) != out.digest().size_bytes()) return false;
    if (((st.st_mode & S_IXUSR) != 0) != out.executable()) return false;
    return store::CasStore::ComputeFileDigest(target).hash == out.digest().hash();
}

bool Write(store::CasStore& cas, store::MerkleBuilder& merkle, const v1::OutputFile& out, const fs::path& target,
           std::string* error) {
    if (!out.is_dir() && !out.symlink() && AlreadyCurrent(target, out)) return true;
    const fs::path tmp = target.parent_path() / (".zm-place-" + RandomHex(8));
    bool ok = false;
    if (out.symlink()) {
        ok = symlink(out.target().c_str(), tmp.c_str()) == 0;
        if (!ok) *error = Errno("cannot create symlink", tmp);
    } else if (out.is_dir()) {
        ok = merkle.MaterializeTree({out.digest().hash(), out.digest().size_bytes()}, tmp, store::Materialize::Copy);
        if (!ok) *error = "cannot materialize directory " + out.path() + " from the store";
    } else {
        if (!cas.HasBlob(out.digest().hash())) {
            *error = "output " + out.path() + " is missing from the store";
            return false;
        }
        fs::path src = cas.GetBlobPath(out.digest().hash());
        if (!fs::exists(src)) {
            if (auto blob = cas.GetBlob(out.digest().hash())) cas.PutBlob(*blob); // write an in-memory blob through
        }
        ok = CopyFile(src, tmp, out.executable(), error);
    }
    if (ok) {
        // rename(2) replaces a file or symlink at the target (never following it); a directory
        // there is removed first.
        struct stat st{};
        if (lstat(target.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            std::error_code ec;
            fs::remove_all(target, ec);
        }
        if (rename(tmp.c_str(), target.c_str()) != 0) {
            *error = Errno("cannot write", target);
            ok = false;
        }
    }
    if (!ok) {
        std::error_code ec;
        fs::remove_all(tmp, ec);
    }
    return ok;
}

} // namespace

bool Place(store::CasStore& cas, store::MerkleBuilder& merkle, const v1::OutputFile& out, const fs::path& dest,
           std::string* error) {
    if (!dest.is_absolute()) {
        *error = "destination must be absolute: " + dest.string();
        return false;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) {
        *error = "cannot create " + dest.parent_path().string() + ": " + ec.message();
        return false;
    }
    return Write(cas, merkle, out, dest, error);
}

bool PlaceUnder(store::CasStore& cas, store::MerkleBuilder& merkle, const v1::OutputFile& out, const fs::path& root,
                std::string* error, fs::path* placed) {
    std::vector<std::string> segs;
    for (const auto& part : fs::path(out.path()).lexically_normal()) {
        const std::string s = part.string();
        if (s.empty() || s == ".") continue;
        if (s == ".." || s == "/") {
            *error = "refusing unsafe output path '" + out.path() + "'";
            return false;
        }
        segs.push_back(s);
    }
    if (segs.empty()) {
        *error = "refusing empty output path";
        return false;
    }
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::path cur = root;
    for (size_t i = 0; i + 1 < segs.size(); ++i) {
        cur /= segs[i];
        struct stat st{};
        if (lstat(cur.c_str(), &st) != 0) {
            if (errno != ENOENT || mkdir(cur.c_str(), 0755) != 0) {
                *error = Errno("cannot create directory", cur);
                return false;
            }
            continue;
        }
        if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
            *error = "refusing to write through " + cur.string();
            return false;
        }
    }
    const fs::path target = cur / segs.back();
    if (placed) *placed = target;
    return Write(cas, merkle, out, target, error);
}

} // namespace zeromesh::run
