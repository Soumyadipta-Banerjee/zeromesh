#include "exec/tree_watch.hpp"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cstring>

namespace zeromesh::exec {

namespace {

constexpr uint32_t kDirMask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE;

} // namespace

TreeWatch::TreeWatch(std::filesystem::path root) : root_(std::move(root)) {
    fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd_ >= 0) AddRecursive(root_);
}

TreeWatch::~TreeWatch() {
    if (fd_ >= 0) close(fd_);
}

void TreeWatch::AddRecursive(const std::filesystem::path& dir) {
    const int wd = inotify_add_watch(fd_, dir.c_str(), kDirMask | IN_ONLYDIR);
    if (wd < 0) return;
    dirs_[wd] = dir;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && !it->is_symlink(ec)) AddRecursive(it->path());
    }
}

std::vector<std::string> TreeWatch::Poll(std::chrono::milliseconds timeout) {
    std::vector<std::string> out;
    if (fd_ < 0) return out;
    pollfd p{fd_, POLLIN, 0};
    if (poll(&p, 1, static_cast<int>(timeout.count())) <= 0) return out;
    alignas(inotify_event) char buf[16384];
    for (;;) {
        const ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t off = 0; off < n;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
            off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);
            auto it = dirs_.find(ev->wd);
            if (it == dirs_.end() || ev->len == 0) continue;
            const std::filesystem::path path = it->second / ev->name;
            if (ev->mask & IN_ISDIR) {
                AddRecursive(path);
                // A directory renamed into place arrives complete: report what it holds.
                if (ev->mask & IN_MOVED_TO) {
                    std::error_code ec;
                    for (std::filesystem::recursive_directory_iterator rit(path, ec), end; !ec && rit != end;
                         rit.increment(ec)) {
                        if (rit->is_regular_file(ec))
                            out.push_back(std::filesystem::relative(rit->path(), root_, ec).string());
                    }
                }
                continue;
            }
            if (ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) {
                std::error_code ec;
                out.push_back(std::filesystem::relative(path, root_, ec).string());
            }
        }
    }
    return out;
}

} // namespace zeromesh::exec
