#include "store/merkle.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

namespace zeromesh::store {

std::string DirectoryManifest::Serialize() const {
    // Make a sorted copy to guarantee deterministic hashing
    auto sorted_files = files;
    std::sort(sorted_files.begin(), sorted_files.end(), [](const FileNode& a, const FileNode& b) {
        return a.name < b.name;
    });

    auto sorted_dirs = directories;
    std::sort(sorted_dirs.begin(), sorted_dirs.end(), [](const DirectoryNode& a, const DirectoryNode& b) {
        return a.name < b.name;
    });

    auto sorted_links = symlinks;
    std::sort(sorted_links.begin(), sorted_links.end(), [](const SymlinkNode& a, const SymlinkNode& b) {
        return a.name < b.name;
    });

    std::ostringstream ss;
    for (const auto& f : sorted_files) {
        ss << "F\t" << f.digest.hash << "\t" << f.digest.size_bytes << "\t" << (f.is_executable ? "1" : "0") << "\t" << f.name << "\n";
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
    }

    return manifest;
}

MerkleBuilder::MerkleBuilder(std::shared_ptr<CasStore> store)
    : store_(std::move(store)) {}

Digest MerkleBuilder::BuildTree(const std::filesystem::path& root_dir) {
    if (!std::filesystem::exists(root_dir) || !std::filesystem::is_directory(root_dir)) {
        return Digest{"", 0};
    }
    return BuildDirectoryRecursive(root_dir);
}

Digest MerkleBuilder::BuildDirectoryRecursive(const std::filesystem::path& current_path) {
    DirectoryManifest manifest;

    for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
        std::string filename = entry.path().filename().string();

        if (entry.is_symlink()) {
            std::string target = std::filesystem::read_symlink(entry.path()).string();
            manifest.symlinks.push_back(SymlinkNode{
                .name = filename,
                .target = target
            });
        } else if (entry.is_directory()) {
            Digest sub_digest = BuildDirectoryRecursive(entry.path());
            manifest.directories.push_back(DirectoryNode{
                .name = filename,
                .digest = sub_digest
            });
        } else if (entry.is_regular_file()) {
            std::ifstream file(entry.path(), std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            Digest file_digest = store_->PutBlob(content);

            auto perms = entry.status().permissions();
            bool is_exec = (perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;

            manifest.files.push_back(FileNode{
                .name = filename,
                .digest = file_digest,
                .is_executable = is_exec
            });
        }
    }

    std::string serialized = manifest.Serialize();
    return store_->PutBlob(serialized);
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

    std::filesystem::create_directories(current_path);

    // Materialize files
    for (const auto& file : manifest->files) {
        auto blob_content = store_->GetBlob(file.digest.hash);
        if (!blob_content.has_value()) {
            return false;
        }

        std::filesystem::path file_path = current_path / file.name;
        std::ofstream out(file_path, std::ios::binary);
        out.write(blob_content->data(), static_cast<std::streamsize>(blob_content->size()));
        out.close();

        if (file.is_executable) {
            std::filesystem::permissions(file_path, std::filesystem::perms::owner_exec | std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
        }
    }

    // Materialize subdirectories
    for (const auto& dir : manifest->directories) {
        std::filesystem::path sub_path = current_path / dir.name;
        if (!MaterializeDirectoryRecursive(dir.digest, sub_path)) {
            return false;
        }
    }

    // Materialize symlinks
    for (const auto& link : manifest->symlinks) {
        std::filesystem::path link_path = current_path / link.name;
        std::error_code ec;
        std::filesystem::create_symlink(link.target, link_path, ec);
    }

    return true;
}

} // namespace zeromesh::store
