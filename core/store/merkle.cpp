#include "store/merkle.hpp"

#include <algorithm>
#include <fstream>
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

Digest MerkleBuilder::BuildTree(const std::filesystem::path& root_dir) {
    return BuildTree(root_dir, IgnoreRules::None(), nullptr);
}

Digest MerkleBuilder::BuildTree(const std::filesystem::path& root_dir, const IgnoreRules& rules,
                                uint64_t* total_bytes) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root_dir, ec)) {
        return Digest{"", 0};
    }
    return BuildDirectoryRecursive(root_dir, "", rules, total_bytes);
}

Digest MerkleBuilder::BuildDirectoryRecursive(const std::filesystem::path& current_path, const std::string& rel,
                                              const IgnoreRules& rules, uint64_t* total_bytes) {
    DirectoryManifest manifest;

    for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
        std::string filename = entry.path().filename().string();
        if (filename.find('\n') != std::string::npos || filename.find('\t') != std::string::npos) {
            throw std::runtime_error("unsupported file name (contains newline or tab): " + entry.path().string());
        }
        std::string child_rel = rel.empty() ? filename : rel + "/" + filename;

        if (entry.is_symlink()) {
            if (rules.Ignored(child_rel, false)) continue;
            manifest.symlinks.push_back(
                SymlinkNode{.name = filename, .target = std::filesystem::read_symlink(entry.path()).string()});
        } else if (entry.is_directory()) {
            if (rules.Ignored(child_rel, true)) continue;
            manifest.directories.push_back(DirectoryNode{
                .name = filename, .digest = BuildDirectoryRecursive(entry.path(), child_rel, rules, total_bytes)});
        } else if (entry.is_regular_file()) {
            if (rules.Ignored(child_rel, false)) continue;
            Digest file_digest = store_->PutBlobFile(entry.path());
            if (total_bytes) *total_bytes += static_cast<uint64_t>(file_digest.size_bytes);
            auto perms = entry.status().permissions();
            manifest.files.push_back(FileNode{.name = filename,
                                              .digest = file_digest,
                                              .is_executable = (perms & std::filesystem::perms::owner_exec) !=
                                                               std::filesystem::perms::none});
        }
    }

    return store_->PutBlob(manifest.Serialize());
}

bool MerkleBuilder::MaterializeTree(const Digest& root_digest, const std::filesystem::path& destination) {
    if (root_digest.hash.empty()) {
        return false;
    }
    std::filesystem::create_directories(destination);
    return MaterializeDirectoryRecursive(root_digest, destination);
}

bool MerkleBuilder::MaterializeDirectoryRecursive(const Digest& dir_digest, const std::filesystem::path& current_path) {
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
            return false;
        }
        std::filesystem::path file_path = current_path / file.name;
        std::filesystem::copy_file(src, file_path, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return false;
        auto mode = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                    std::filesystem::perms::group_read | std::filesystem::perms::others_read;
        if (file.is_executable) {
            mode |= std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                    std::filesystem::perms::others_exec;
        }
        std::filesystem::permissions(file_path, mode, std::filesystem::perm_options::replace, ec);
    }

    for (const auto& dir : manifest->directories) {
        if (!MaterializeDirectoryRecursive(dir.digest, current_path / dir.name)) {
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
