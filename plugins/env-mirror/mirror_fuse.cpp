#define FUSE_USE_VERSION 31

#include "plugins/env-mirror/mirror_fuse.hpp"
#include "store/merkle.hpp"

#include <fuse.h>
#include <fuse_lowlevel.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace zeromesh::mirror {

namespace {

std::vector<std::string> SplitPath(std::string_view p) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < p.size()) {
        while (start < p.size() && p[start] == '/') ++start;
        if (start >= p.size()) break;
        size_t end = p.find('/', start);
        if (end == std::string_view::npos) end = p.size();
        parts.emplace_back(p.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

} // namespace

struct MirrorFuse::Impl {
    std::filesystem::path mount_point;
    std::shared_ptr<IChunkSource> chunk_source;
    std::shared_ptr<MirrorCache> cache;

    std::shared_ptr<VirtualNode> root;
    std::mutex tree_mu;

    struct fuse* fuse_handle{nullptr};
    std::thread fuse_thread;
    std::atomic<bool> mounted{false};

    std::shared_ptr<VirtualNode> FindNode(std::string_view path) {
        std::lock_guard<std::mutex> lock(tree_mu);
        if (!root) return nullptr;

        auto parts = SplitPath(path);
        auto curr = root;

        for (const auto& part : parts) {
            if (!curr->is_directory) return nullptr;

            // If children not yet loaded and directory has a digest, lazy-load it
            if (curr->children.empty() && !curr->digest_hash.empty()) {
                LoadDirectoryChildren(curr);
            }

            auto it = curr->children.find(part);
            if (it == curr->children.end()) return nullptr;
            curr = it->second;
        }

        return curr;
    }

    void LoadDirectoryChildren(std::shared_ptr<VirtualNode> dir_node) {
        if (!dir_node || dir_node->digest_hash.empty()) return;

        std::optional<std::string> data;
        if (cache) {
            data = cache->Get(dir_node->digest_hash);
        }
        if (!data && chunk_source) {
            data = chunk_source->FetchChunk(dir_node->digest_hash);
            if (data && cache) {
                cache->Put(dir_node->digest_hash, *data);
            }
        }
        if (!data) return;

        auto manifest = store::DirectoryManifest::Deserialize(*data);
        if (!manifest) return;

        for (const auto& f : manifest->files) {
            auto child = std::make_shared<VirtualNode>();
            child->name = f.name;
            child->is_directory = false;
            child->is_executable = f.is_executable;
            child->size_bytes = f.digest.size_bytes;
            child->digest_hash = f.digest.hash;
            dir_node->children[f.name] = child;
        }

        for (const auto& s : manifest->symlinks) {
            auto child = std::make_shared<VirtualNode>();
            child->name = s.name;
            child->is_symlink = true;
            child->symlink_target = s.target;
            dir_node->children[s.name] = child;
        }

        for (const auto& d : manifest->directories) {
            auto child = std::make_shared<VirtualNode>();
            child->name = d.name;
            child->is_directory = true;
            child->digest_hash = d.digest.hash;
            dir_node->children[d.name] = child;
        }
    }
};

namespace {

MirrorFuse::Impl* GetImpl() {
    return static_cast<MirrorFuse::Impl*>(fuse_get_context()->private_data);
}

int FuseGetattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi) {
    (void)fi;
    auto* impl = GetImpl();
    if (!impl) return -EIO;

    auto node = impl->FindNode(path);
    if (!node) return -ENOENT;

    std::memset(stbuf, 0, sizeof(struct stat));

    if (node->is_directory) {
        stbuf->st_mode = S_IFDIR | 0555;
        stbuf->st_nlink = 2;
    } else if (node->is_symlink) {
        stbuf->st_mode = S_IFLNK | 0777;
        stbuf->st_nlink = 1;
        stbuf->st_size = static_cast<off_t>(node->symlink_target.size());
    } else {
        stbuf->st_mode = S_IFREG | (node->is_executable ? 0555 : 0444);
        stbuf->st_nlink = 1;
        stbuf->st_size = static_cast<off_t>(node->size_bytes);
    }

    stbuf->st_uid = ::getuid();
    stbuf->st_gid = ::getgid();
    return 0;
}

int FuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler,
                off_t offset, struct fuse_file_info* fi, enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    auto* impl = GetImpl();
    if (!impl) return -EIO;

    auto node = impl->FindNode(path);
    if (!node || !node->is_directory) return -ENOENT;

    filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));

    std::lock_guard<std::mutex> lock(impl->tree_mu);
    if (node->children.empty() && !node->digest_hash.empty()) {
        impl->LoadDirectoryChildren(node);
    }

    for (const auto& [name, child] : node->children) {
        filler(buf, name.c_str(), nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
    }

    return 0;
}

int FuseOpen(const char* path, struct fuse_file_info* fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EROFS;
    }

    auto* impl = GetImpl();
    if (!impl) return -EIO;

    auto node = impl->FindNode(path);
    if (!node) return -ENOENT;
    if (node->is_directory) return -EISDIR;

    return 0;
}

int FuseRead(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    (void)fi;
    auto* impl = GetImpl();
    if (!impl) return -EIO;

    auto node = impl->FindNode(path);
    if (!node) return -ENOENT;
    if (node->is_directory) return -EISDIR;

    if (offset < 0 || static_cast<uint64_t>(offset) >= node->size_bytes) {
        return 0;
    }

    size_t available = static_cast<size_t>(node->size_bytes - offset);
    size = std::min(size, available);
    if (size == 0) return 0;

    if (impl->cache) {
        if (auto n = impl->cache->ReadRange(node->digest_hash, offset, buf, size)) {
            return static_cast<int>(*n);
        }
    }
    if (!impl->chunk_source) return -EIO;

    std::optional<std::string> file_data = impl->chunk_source->FetchChunk(node->digest_hash);
    if (!file_data) return -EIO;
    if (impl->cache) impl->cache->Put(node->digest_hash, *file_data);

    if (static_cast<size_t>(offset) >= file_data->size()) return 0;
    size_t tail = file_data->size() - static_cast<size_t>(offset);
    size = std::min(size, tail);
    std::memcpy(buf, file_data->data() + offset, size);
    return static_cast<int>(size);
}

int FuseReadlink(const char* path, char* buf, size_t size) {
    auto* impl = GetImpl();
    if (!impl) return -EIO;

    auto node = impl->FindNode(path);
    if (!node || !node->is_symlink) return -ENOENT;

    size_t copy_len = std::min(size - 1, node->symlink_target.size());
    std::memcpy(buf, node->symlink_target.data(), copy_len);
    buf[copy_len] = '\0';
    return 0;
}

// All modifying operations strictly return -EROFS
int FuseWrite(const char*, const char*, size_t, off_t, struct fuse_file_info*) { return -EROFS; }
int FuseCreate(const char*, mode_t, struct fuse_file_info*) { return -EROFS; }
int FuseMkdir(const char*, mode_t) { return -EROFS; }
int FuseUnlink(const char*) { return -EROFS; }
int FuseRmdir(const char*) { return -EROFS; }
int FuseRename(const char*, const char*, unsigned int) { return -EROFS; }
int FuseTruncate(const char*, off_t, struct fuse_file_info*) { return -EROFS; }
int FuseChmod(const char*, mode_t, struct fuse_file_info*) { return -EROFS; }
int FuseChown(const char*, uid_t, gid_t, struct fuse_file_info*) { return -EROFS; }
int FuseUtimens(const char*, const struct timespec[2], struct fuse_file_info*) { return -EROFS; }

struct fuse_operations MakeFuseOps() {
    struct fuse_operations ops{};
    ops.getattr  = FuseGetattr;
    ops.readdir  = FuseReaddir;
    ops.open     = FuseOpen;
    ops.read     = FuseRead;
    ops.readlink = FuseReadlink;
    ops.write    = FuseWrite;
    ops.create   = FuseCreate;
    ops.mkdir    = FuseMkdir;
    ops.unlink   = FuseUnlink;
    ops.rmdir    = FuseRmdir;
    ops.rename   = FuseRename;
    ops.truncate = FuseTruncate;
    ops.chmod    = FuseChmod;
    ops.chown    = FuseChown;
    ops.utimens  = FuseUtimens;
    return ops;
}

} // namespace

MirrorFuse::MirrorFuse(std::filesystem::path mount_point,
                       std::shared_ptr<IChunkSource> chunk_source,
                       std::shared_ptr<MirrorCache> cache)
    : mount_point_(std::move(mount_point))
    , chunk_source_(std::move(chunk_source))
    , cache_(std::move(cache))
    , impl_(std::make_unique<Impl>()) {
    impl_->mount_point = mount_point_;
    impl_->chunk_source = chunk_source_;
    impl_->cache = cache_;
}

MirrorFuse::~MirrorFuse() {
    Unmount();
}

bool MirrorFuse::IsFuseAvailable() {
    return ::access("/dev/fuse", R_OK | W_OK) == 0;
}

bool MirrorFuse::IsPathMounted(const std::filesystem::path& path) {
    std::ifstream in("/proc/self/mounts");
    if (!in.is_open()) return false;

    std::string canonical_target;
    std::error_code ec;
    auto can_p = std::filesystem::canonical(path, ec);
    canonical_target = ec ? path.string() : can_p.string();

    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string device, mnt;
        if (ss >> device >> mnt) {
            if (mnt == path.string()) return true;
            auto can_mnt = std::filesystem::canonical(mnt, ec);
            if (!ec && can_mnt.string() == canonical_target) return true;
        }
    }
    return false;
}

bool MirrorFuse::FusermountUnmountLazy(const std::filesystem::path& mount_point) {
    const std::string path = mount_point.string();
    if (path.empty()) return false;

    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        char arg0[] = "fusermount3";
        char arg1[] = "-u";
        char arg2[] = "-z";
        std::vector<char> arg3(path.begin(), path.end());
        arg3.push_back('\0');
        char* argv[] = {arg0, arg1, arg2, arg3.data(), nullptr};
        execvp("fusermount3", argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool MirrorFuse::Mount(const std::string& root_manifest_digest) {
    auto root = std::make_shared<VirtualNode>();
    root->name = "";
    root->is_directory = true;
    root->digest_hash = root_manifest_digest;
    impl_->LoadDirectoryChildren(root);
    return MountWithRoot(std::move(root));
}

bool MirrorFuse::MountWithRoot(std::shared_ptr<VirtualNode> root_node) {
    if (impl_->mounted.load()) return true;

    std::error_code ec;
    std::filesystem::create_directories(mount_point_, ec);

    impl_->root = std::move(root_node);

    static auto ops = MakeFuseOps();
    char* argv[] = {const_cast<char*>("zm_mirror_fuse"), nullptr};
    struct fuse_args args = FUSE_ARGS_INIT(1, argv);

    impl_->fuse_handle = fuse_new(&args, &ops, sizeof(ops), impl_.get());
    fuse_opt_free_args(&args);

    if (!impl_->fuse_handle) {
        return false;
    }

    if (fuse_mount(impl_->fuse_handle, mount_point_.c_str()) != 0) {
        fuse_destroy(impl_->fuse_handle);
        impl_->fuse_handle = nullptr;
        return false;
    }

    impl_->mounted.store(true);
    impl_->fuse_thread = std::thread([this]() {
        fuse_loop(impl_->fuse_handle);
    });

    // Wait briefly for mount to settle
    for (int i = 0; i < 50; ++i) {
        if (IsPathMounted(mount_point_)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return true;
}

void MirrorFuse::Unmount() {
    if (!impl_->mounted.exchange(false)) return;

    if (impl_->fuse_handle) {
        struct fuse_session* se = fuse_get_session(impl_->fuse_handle);
        if (se) {
            fuse_session_exit(se);
        }
        fuse_unmount(impl_->fuse_handle);
        if (impl_->fuse_thread.joinable()) {
            impl_->fuse_thread.join();
        }
        fuse_destroy(impl_->fuse_handle);
        impl_->fuse_handle = nullptr;
    }

    // Ensure mount is truly unmounted from kernel namespace
    if (IsPathMounted(mount_point_)) {
        ::umount2(mount_point_.c_str(), MNT_DETACH);
        if (IsPathMounted(mount_point_)) {
            FusermountUnmountLazy(mount_point_);
        }
    }

    std::error_code ec;
    std::filesystem::remove(mount_point_, ec);
}

bool MirrorFuse::IsMounted() const {
    return impl_->mounted.load();
}

std::shared_ptr<VirtualNode> MirrorFuse::Lookup(std::string_view path) {
    return impl_->FindNode(path);
}

} // namespace zeromesh::mirror
