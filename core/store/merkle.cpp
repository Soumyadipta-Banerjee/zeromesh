#include "store/merkle.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace zeromesh::store {

std::string DirectoryManifest::Serialize() const {
    // Make a sorted copy to guarantee deterministic hashing
    auto sorted_files = files;
    std::sort(sorted_files.begin(), sorted_files.end(),
              [](const FileNode& a, const FileNode& b) { return a.name < b.name; });

    auto sorted_dirs = directories;
    std::sort(sorted_dirs.begin(), sorted_dirs.end(),
              [](const DirectoryNode& a, const DirectoryNode& b) { return a.name < b.name; });

    auto sorted_links = symlinks;
    std::sort(sorted_links.begin(), sorted_links.end(),
              [](const SymlinkNode& a, const SymlinkNode& b) { return a.name < b.name; });

    std::ostringstream ss;
    for (const auto& f : sorted_files) {
        ss << "F\t" << f.digest.hash << "\t" << f.digest.size_bytes << "\t" << (f.is_executable ? "1" : "0") << "\t"
           << f.name << "\n";
    }
    for (const auto& d : sorted_dirs) {
        ss << "D\t" << d.digest.hash << "\t" << d.digest.size_bytes << "\t" << d.name << "\n";
    }
    for (const auto& l : sorted_links) {
        ss << "S\t" << l.target << "\t" << l.name << "\n";
    }
    return ss.str();
}

std::optional<DirectoryManifest> DirectoryManifest::Deserialize(std::string_view data) {
    DirectoryManifest manifest;
    std::istringstream ss{std::string(data)};
    std::string line;

    while (std::getline(ss, line)) {
        if (line.empty()) continue;

        std::vector<std::string> tokens;
        size_t start = 0;
        while (start < line.size()) {
            size_t tab = line.find('\t', start);
            if (tab == std::string::npos) {
                tokens.push_back(line.substr(start));
                break;
            }
            tokens.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }

        if (tokens.empty()) continue;

        try {
            if (tokens[0] == "F" && tokens.size() >= 5) {
                FileNode fn;
                fn.digest.hash = tokens[1];
                fn.digest.size_bytes = std::stoull(tokens[2]);
                fn.is_executable = (tokens[3] == "1");
                fn.name = tokens[4];
                manifest.files.push_back(std::move(fn));
            } else if (tokens[0] == "D" && tokens.size() >= 4) {
                DirectoryNode dn;
                dn.digest.hash = tokens[1];
                dn.digest.size_bytes = std::stoull(tokens[2]);
                dn.name = tokens[3];
                manifest.directories.push_back(std::move(dn));
            } else if (tokens[0] == "S" && tokens.size() >= 3) {
                SymlinkNode sn;
                sn.target = tokens[1];
                sn.name = tokens[2];
                manifest.symlinks.push_back(std::move(sn));
            }
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    return manifest;
}

MerkleBuilder::MerkleBuilder(std::shared_ptr<CasStore> store) : store_(std::move(store)) {}

bool MerkleBuilder::IsSafeName(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    return name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos;
}

bool MerkleBuilder::Skipped(const std::string& rel, bool is_dir, const TreeOptions& opts) {
    if (opts.ignore && opts.ignore->IgnoredEntry(rel, is_dir)) return true;
    if (opts.deny && opts.deny->IgnoredEntry(rel, is_dir)) {
        if (opts.denied) opts.denied->push_back(rel);
        return true;
    }
    return false;
}

Digest MerkleBuilder::HashFile(const std::filesystem::path& path, const TreeOptions& opts) {
    struct stat st{};
    const bool have_stat = ::stat(path.c_str(), &st) == 0;
    if (opts.stat_cache && have_stat) {
        if (auto cached = opts.stat_cache->Lookup(path.string(), st); cached && store_->HasBlob(cached->hash)) {
            return *cached;
        }
    }
    Digest d = store_->PutBlobFile(path);
    if (d.hash.empty()) throw std::runtime_error("cannot read " + path.string());
    if (opts.stat_cache && have_stat) opts.stat_cache->Store(path.string(), st, d);
    return d;
}

Digest MerkleBuilder::BuildTree(const std::filesystem::path& root_dir, const TreeOptions& opts) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root_dir, ec)) {
        return Digest{"", 0};
    }
    return BuildDirectoryRecursive(root_dir, "", opts);
}

Digest MerkleBuilder::BuildDirectoryRecursive(const std::filesystem::path& current_path, const std::string& rel,
                                              const TreeOptions& opts) {
    DirectoryManifest manifest;

    for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
        std::string filename = entry.path().filename().string();
        if (filename.find('\n') != std::string::npos || filename.find('\t') != std::string::npos) {
            throw std::runtime_error("unsupported file name (contains newline or tab): " + entry.path().string());
        }
        std::string child_rel = rel.empty() ? filename : rel + "/" + filename;
        if (entry.is_symlink()) {
            if (Skipped(child_rel, false, opts)) continue;
            manifest.symlinks.push_back(
                SymlinkNode{.name = filename, .target = std::filesystem::read_symlink(entry.path()).string()});
        } else if (entry.is_directory()) {
            if (Skipped(child_rel, true, opts)) continue;
            manifest.directories.push_back(
                DirectoryNode{.name = filename, .digest = BuildDirectoryRecursive(entry.path(), child_rel, opts)});
        } else if (entry.is_regular_file()) {
            if (Skipped(child_rel, false, opts)) continue;
            Digest file_digest = HashFile(entry.path(), opts);
            if (opts.total_bytes) *opts.total_bytes += static_cast<uint64_t>(file_digest.size_bytes);
            if (opts.file_count) *opts.file_count += 1;
            auto perms = entry.status().permissions();
            manifest.files.push_back(FileNode{.name = filename,
                                              .digest = file_digest,
                                              .is_executable = (perms & std::filesystem::perms::owner_exec) !=
                                                               std::filesystem::perms::none});
        }
    }

    return store_->PutBlob(manifest.Serialize());
}

// ---- BuildFromPaths ---------------------------------------------------------------------------

struct MerkleBuilder::PathNode {
    enum class Kind { Directory, File, Symlink };
    Kind kind{Kind::Directory};
    Digest digest;
    bool executable{false};
    std::string target;
    std::map<std::string, std::unique_ptr<PathNode>> children;

    PathNode* Child(const std::string& name, Kind k) {
        auto& slot = children[name];
        if (!slot) {
            slot = std::make_unique<PathNode>();
            slot->kind = k;
        }
        return slot.get();
    }
};

void MerkleBuilder::InsertDirectory(PathNode& node, const std::filesystem::path& dir, const std::string& rel,
                                    const TreeOptions& opts) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec),
         end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.find_first_of("\n\t") != std::string::npos) continue;
        const std::string child_rel = rel.empty() ? name : rel + "/" + name;
        struct stat st{};
        if (lstat(it->path().c_str(), &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) {
            if (Skipped(child_rel, false, opts)) continue;
            std::error_code lec;
            auto target = std::filesystem::read_symlink(it->path(), lec);
            if (lec) continue;
            node.Child(name, PathNode::Kind::Symlink)->target = target.string();
        } else if (S_ISDIR(st.st_mode)) {
            if (Skipped(child_rel, true, opts)) continue;
            PathNode* child = node.Child(name, PathNode::Kind::Directory);
            if (child->kind != PathNode::Kind::Directory) continue;
            InsertDirectory(*child, it->path(), child_rel, opts);
        } else if (S_ISREG(st.st_mode)) {
            if (Skipped(child_rel, false, opts)) continue;
            PathNode* child = node.Child(name, PathNode::Kind::File);
            if (!child->digest.hash.empty()) continue;
            child->digest = HashFile(it->path(), opts);
            child->executable = (st.st_mode & S_IXUSR) != 0;
            if (opts.total_bytes) *opts.total_bytes += static_cast<uint64_t>(child->digest.size_bytes);
            if (opts.file_count) *opts.file_count += 1;
        }
    }
}

void MerkleBuilder::InsertPath(PathNode& root, const std::filesystem::path& abs, const TreeOptions& opts, int depth) {
    if (depth > 40 || !abs.is_absolute()) return; // a symlink loop, or nothing to anchor
    const std::filesystem::path norm = abs.lexically_normal();
    std::vector<std::string> parts;
    for (const auto& p : norm.relative_path()) {
        const std::string s = p.string();
        if (s.empty() || s == ".") continue;
        if (s == ".." || s.find_first_of("\n\t") != std::string::npos) return;
        parts.push_back(s);
    }
    PathNode* node = &root;
    std::filesystem::path fs_path = "/";
    std::string rel;
    for (size_t i = 0; i < parts.size(); ++i) {
        fs_path /= parts[i];
        rel = rel.empty() ? parts[i] : rel + "/" + parts[i];
        const bool last = i + 1 == parts.size();
        struct stat st{};
        if (lstat(fs_path.c_str(), &st) != 0) return;
        if (S_ISLNK(st.st_mode)) {
            if (Skipped(rel, false, opts)) return;
            std::error_code ec;
            const auto target = std::filesystem::read_symlink(fs_path, ec);
            if (ec) return;
            PathNode* link = node->Child(parts[i], PathNode::Kind::Symlink);
            if (link->kind != PathNode::Kind::Symlink) return;
            link->target = target.string();
            std::filesystem::path resolved = target.is_absolute() ? target : fs_path.parent_path() / target;
            for (size_t j = i + 1; j < parts.size(); ++j) resolved /= parts[j];
            InsertPath(root, resolved, opts, depth + 1);
            return;
        }
        if (S_ISDIR(st.st_mode)) {
            if (Skipped(rel, true, opts)) return;
            PathNode* dir = node->Child(parts[i], PathNode::Kind::Directory);
            if (dir->kind != PathNode::Kind::Directory) return;
            if (last) InsertDirectory(*dir, fs_path, rel, opts);
            node = dir;
            continue;
        }
        if (S_ISREG(st.st_mode) && last) {
            if (Skipped(rel, false, opts)) return;
            PathNode* file = node->Child(parts[i], PathNode::Kind::File);
            if (file->kind != PathNode::Kind::File || !file->digest.hash.empty()) return;
            file->digest = HashFile(fs_path, opts);
            file->executable = (st.st_mode & S_IXUSR) != 0;
            if (opts.total_bytes) *opts.total_bytes += static_cast<uint64_t>(file->digest.size_bytes);
            if (opts.file_count) *opts.file_count += 1;
        }
        return; // a path through a file, or a device/socket
    }
}

Digest MerkleBuilder::StoreNode(const PathNode& node) {
    DirectoryManifest m;
    for (const auto& [name, child] : node.children) {
        switch (child->kind) {
        case PathNode::Kind::Directory:
            m.directories.push_back({.name = name, .digest = StoreNode(*child)});
            break;
        case PathNode::Kind::File:
            if (!child->digest.hash.empty())
                m.files.push_back({.name = name, .digest = child->digest, .is_executable = child->executable});
            break;
        case PathNode::Kind::Symlink:
            m.symlinks.push_back({.name = name, .target = child->target});
            break;
        }
    }
    return store_->PutBlob(m.Serialize());
}

Digest MerkleBuilder::BuildFromPaths(const std::vector<std::filesystem::path>& abs_paths, const TreeOptions& opts) {
    PathNode root;
    for (const auto& p : abs_paths) InsertPath(root, p, opts, 0);
    return StoreNode(root);
}

// ---- Materialize ------------------------------------------------------------------------------

bool MerkleBuilder::MaterializeTree(const Digest& root_digest, const std::filesystem::path& destination,
                                    Materialize mode) {
    if (root_digest.hash.empty()) {
        return false;
    }
    std::filesystem::create_directories(destination);
    return MaterializeDirectoryRecursive(root_digest, destination, mode);
}

bool MerkleBuilder::MaterializeDirectoryRecursive(const Digest& dir_digest, const std::filesystem::path& current_path,
                                                  Materialize mode) {
    auto manifest_data = store_->GetBlob(dir_digest.hash);
    if (!manifest_data.has_value()) {
        return false;
    }

    auto manifest = DirectoryManifest::Deserialize(*manifest_data);
    if (!manifest.has_value()) {
        return false;
    }

    // Validate every name before touching the disk, so a hostile manifest creates nothing.
    for (const auto& f : manifest->files)
        if (!IsSafeName(f.name)) return false;
    for (const auto& d : manifest->directories)
        if (!IsSafeName(d.name)) return false;
    for (const auto& l : manifest->symlinks)
        if (!IsSafeName(l.name)) return false;

    std::filesystem::create_directories(current_path);

    for (const auto& file : manifest->files) {
        auto src = store_->GetBlobPath(file.digest.hash);
        std::error_code ec;
        if (src.empty() || !std::filesystem::exists(src, ec)) {
            // Small blobs can live only in the in-memory cache: write them through to disk.
            auto blob = store_->GetBlob(file.digest.hash);
            if (!blob) return false;
            store_->PutBlob(*blob);
            if (!std::filesystem::exists(src, ec)) return false;
        }
        std::filesystem::path file_path = current_path / file.name;
        // A link shares the store's inode, so it is only made when the exec bit already agrees:
        // changing the mode of the link would change the blob for every other user.
        if (mode == Materialize::Link) {
            struct stat st{};
            if (::stat(src.c_str(), &st) == 0 && ((st.st_mode & S_IXUSR) != 0) == file.is_executable) {
                std::filesystem::create_hard_link(src, file_path, ec);
                if (!ec) continue;
                ec.clear();
            }
        }
        std::filesystem::copy_file(src, file_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
        auto perms = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                     std::filesystem::perms::group_read | std::filesystem::perms::others_read;
        if (file.is_executable) {
            perms |= std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                     std::filesystem::perms::others_exec;
        }
        std::filesystem::permissions(file_path, perms, std::filesystem::perm_options::replace, ec);
        if (ec) return false;
    }

    for (const auto& dir : manifest->directories) {
        if (!MaterializeDirectoryRecursive(dir.digest, current_path / dir.name, mode)) {
            return false;
        }
    }

    // Symlinks last, so no file or directory above is ever written through one.
    for (const auto& link : manifest->symlinks) {
        std::error_code ec;
        std::filesystem::create_symlink(link.target, current_path / link.name, ec);
        if (ec) return false;
    }

    return true;
}

// ---- Reading trees ----------------------------------------------------------------------------

TreeEntry MerkleBuilder::Lookup(const Digest& root, std::string_view rel) {
    TreeEntry cur;
    cur.kind = TreeEntry::Kind::Directory;
    cur.digest = root;
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        if (slash == std::string_view::npos) slash = rel.size();
        const std::string_view seg = rel.substr(start, slash - start);
        start = slash + 1;
        if (seg.empty() || seg == ".") continue;
        if (cur.kind != TreeEntry::Kind::Directory || seg == "..") return {};
        const DirectoryManifest m = LoadManifest(cur.digest);
        TreeEntry next;
        for (const auto& f : m.files) {
            if (f.name == seg) next = {TreeEntry::Kind::File, f.digest, f.is_executable, {}};
        }
        for (const auto& d : m.directories) {
            if (d.name == seg) next = {TreeEntry::Kind::Directory, d.digest, false, {}};
        }
        for (const auto& l : m.symlinks) {
            if (l.name == seg) next = {TreeEntry::Kind::Symlink, {}, false, l.target};
        }
        if (next.kind == TreeEntry::Kind::Absent) return {};
        cur = std::move(next);
    }
    if (cur.kind == TreeEntry::Kind::Directory && cur.digest.hash.empty()) return {};
    return cur;
}

std::optional<std::vector<std::string>> MerkleBuilder::ListNames(const Digest& root, std::string_view rel) {
    const TreeEntry e = Lookup(root, rel);
    if (e.kind != TreeEntry::Kind::Directory) return std::nullopt;
    const DirectoryManifest m = LoadManifest(e.digest);
    std::vector<std::string> names;
    for (const auto& f : m.files) names.push_back(f.name);
    for (const auto& d : m.directories) names.push_back(d.name);
    for (const auto& l : m.symlinks) names.push_back(l.name);
    std::sort(names.begin(), names.end());
    return names;
}

void MerkleBuilder::DiffRecursive(const Digest& a, const Digest& b, const std::string& prefix, size_t limit,
                                  std::vector<std::string>& out) {
    if (out.size() >= limit || a.hash == b.hash) return;
    struct Entry {
        TreeEntry::Kind kind;
        std::string id; // digest+mode or link target
        Digest digest;
    };
    auto index = [&](const Digest& tree) {
        std::map<std::string, Entry> m;
        if (tree.hash.empty()) return m;
        const DirectoryManifest man = LoadManifest(tree);
        for (const auto& f : man.files)
            m[f.name] = {TreeEntry::Kind::File, f.digest.hash + (f.is_executable ? "+x" : ""), f.digest};
        for (const auto& d : man.directories) m[d.name] = {TreeEntry::Kind::Directory, d.digest.hash, d.digest};
        for (const auto& l : man.symlinks) m[l.name] = {TreeEntry::Kind::Symlink, l.target, {}};
        return m;
    };
    const auto ea = index(a);
    const auto eb = index(b);
    std::set<std::string> names;
    for (const auto& [n, _] : ea) names.insert(n);
    for (const auto& [n, _] : eb) names.insert(n);
    for (const auto& name : names) {
        if (out.size() >= limit) return;
        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        auto ia = ea.find(name);
        auto ib = eb.find(name);
        if (ia != ea.end() && ib != eb.end() && ia->second.kind == ib->second.kind && ia->second.id == ib->second.id) {
            continue;
        }
        if (ia != ea.end() && ib != eb.end() && ia->second.kind == TreeEntry::Kind::Directory &&
            ib->second.kind == TreeEntry::Kind::Directory) {
            DiffRecursive(ia->second.digest, ib->second.digest, path, limit, out);
        } else {
            out.push_back(path);
        }
    }
}

std::vector<std::string> MerkleBuilder::Diff(const Digest& a, const Digest& b, size_t limit) {
    std::vector<std::string> out;
    if (limit == 0) return out;
    DiffRecursive(a, b, "", limit, out);
    return out;
}

DirectoryManifest MerkleBuilder::LoadManifest(const Digest& tree) {
    if (tree.hash.empty()) return {};
    auto data = store_->GetBlob(tree.hash);
    if (!data) throw std::runtime_error("missing tree " + tree.hash);
    auto m = DirectoryManifest::Deserialize(*data);
    if (!m) throw std::runtime_error("corrupt tree " + tree.hash);
    return *m;
}

std::vector<Digest> MerkleBuilder::CollectTree(const Digest& root) {
    std::vector<Digest> out;
    if (root.hash.empty()) return out;
    std::unordered_set<std::string> seen{root.hash};
    std::vector<Digest> trees{root};
    out.push_back(root);
    while (!trees.empty()) {
        Digest tree = std::move(trees.back());
        trees.pop_back();
        DirectoryManifest m = LoadManifest(tree);
        for (const auto& d : m.directories) {
            if (seen.insert(d.digest.hash).second) {
                out.push_back(d.digest);
                trees.push_back(d.digest);
            }
        }
        for (const auto& f : m.files) {
            if (seen.insert(f.digest.hash).second) out.push_back(f.digest);
        }
    }
    return out;
}

namespace {

void RemoveName(DirectoryManifest& m, const std::string& name) {
    std::erase_if(m.files, [&](const FileNode& f) { return f.name == name; });
    std::erase_if(m.directories, [&](const DirectoryNode& d) { return d.name == name; });
    std::erase_if(m.symlinks, [&](const SymlinkNode& l) { return l.name == name; });
}

const DirectoryNode* FindDir(const DirectoryManifest& m, const std::string& name) {
    for (const auto& d : m.directories) {
        if (d.name == name) return &d;
    }
    return nullptr;
}

} // namespace

Digest MerkleBuilder::MergeTrees(const Digest& base, const Digest& item) {
    DirectoryManifest merged = LoadManifest(base);
    DirectoryManifest add = LoadManifest(item);
    for (const auto& d : add.directories) {
        const DirectoryNode* existing = FindDir(merged, d.name);
        Digest sub = existing ? MergeTrees(existing->digest, d.digest) : d.digest;
        RemoveName(merged, d.name);
        merged.directories.push_back({.name = d.name, .digest = sub});
    }
    for (const auto& f : add.files) {
        RemoveName(merged, f.name);
        merged.files.push_back(f);
    }
    for (const auto& l : add.symlinks) {
        RemoveName(merged, l.name);
        merged.symlinks.push_back(l);
    }
    return store_->PutBlob(merged.Serialize());
}

Digest MerkleBuilder::OverlayRecursive(const Digest& base, const std::vector<std::string>& segs, size_t i,
                                       const Digest& item, bool item_is_tree, bool item_executable) {
    DirectoryManifest m = LoadManifest(base);
    const std::string& name = segs[i];
    const DirectoryNode* existing = FindDir(m, name);
    Digest existing_dir = existing ? existing->digest : Digest{"", 0};

    if (i + 1 == segs.size()) {
        if (item_is_tree) {
            Digest sub = existing_dir.hash.empty() ? item : MergeTrees(existing_dir, item);
            RemoveName(m, name);
            m.directories.push_back({.name = name, .digest = sub});
        } else {
            RemoveName(m, name);
            m.files.push_back({.name = name, .digest = item, .is_executable = item_executable});
        }
    } else {
        Digest sub = OverlayRecursive(existing_dir, segs, i + 1, item, item_is_tree, item_executable);
        RemoveName(m, name);
        m.directories.push_back({.name = name, .digest = sub});
    }
    return store_->PutBlob(m.Serialize());
}

Digest MerkleBuilder::Overlay(const Digest& base_tree, const std::string& at_path, const Digest& item,
                              bool item_is_tree, bool item_executable) {
    std::vector<std::string> segs;
    size_t start = 0;
    while (start <= at_path.size()) {
        size_t slash = at_path.find('/', start);
        if (slash == std::string::npos) slash = at_path.size();
        if (slash > start) segs.push_back(at_path.substr(start, slash - start));
        start = slash + 1;
    }
    for (const auto& s : segs) {
        if (!IsSafeName(s)) throw std::invalid_argument("unsafe overlay path: " + at_path);
    }
    if (segs.empty()) {
        if (!item_is_tree) throw std::invalid_argument("cannot overlay a file at the tree root");
        return base_tree.hash.empty() ? item : MergeTrees(base_tree, item);
    }
    return OverlayRecursive(base_tree, segs, 0, item, item_is_tree, item_executable);
}

} // namespace zeromesh::store
