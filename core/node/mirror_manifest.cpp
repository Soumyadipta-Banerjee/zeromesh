#include "node/mirror_manifest.hpp"

#include "plugins/env-mirror/deny_list.hpp"

#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace zeromesh::node {

namespace fs = std::filesystem;

std::vector<fs::path> ParseMirrorPaths(const std::map<std::string, std::string>& params) {
    std::vector<fs::path> out;
    auto it = params.find(kMirrorPathsParam);
    if (it == params.end() || it->second.empty()) return out;
    std::istringstream in(it->second);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        fs::path p(line);
        if (!p.is_absolute()) throw std::invalid_argument("mirror path must be absolute: " + line);
        out.push_back(std::move(p));
    }
    return out;
}

store::Digest BuildMirrorManifest(store::MerkleBuilder& merkle, const std::vector<fs::path>& paths,
                                  uint64_t* total_bytes) {
    if (paths.empty()) throw std::invalid_argument("mirror requires at least one path");
    auto rules = mirror::DenyList::CreateIgnoreRules();
    uint64_t total = 0;
    store::Digest root;
    for (const fs::path& path : paths) {
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            throw std::invalid_argument("mirror path does not exist: " + path.string());
        }
        store::Digest tree = merkle.BuildTree(path, rules, &total);
        std::string mount_name = path.filename().string();
        if (mount_name.empty()) mount_name = path.string();
        for (char& c : mount_name) {
            if (c == '/') c = '_';
        }
        if (paths.size() == 1) {
            root = tree;
        } else {
            root = merkle.Overlay(root, mount_name, tree, true);
        }
    }
    if (total_bytes) *total_bytes = total;
    return root;
}

} // namespace zeromesh::node
