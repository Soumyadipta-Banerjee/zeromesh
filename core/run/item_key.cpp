#include "run/item_key.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace zeromesh::run {

namespace {

// Length-prefixed fields: no delimiter can make two different keys collide.
class KeyWriter {
public:
    void Str(const std::string& s) {
        uint64_t n = s.size();
        buf_.append(reinterpret_cast<const char*>(&n), sizeof(n));
        buf_.append(s);
    }
    void Tag(char t) { buf_.push_back(t); }
    [[nodiscard]] std::string Hash() const { return store::CasStore::ComputeDigest(buf_).hash; }

private:
    std::string buf_;
};

// The value of a path in a tree: "f:<hash>[:x]", "d:<hash>", "l:<target>", or "" when absent.
std::string ValueAt(store::MerkleBuilder& merkle, const store::Digest& root, const std::string& path) {
    const store::TreeEntry e = merkle.Lookup(root, path);
    switch (e.kind) {
    case store::TreeEntry::Kind::File:
        return "f:" + e.digest.hash + (e.executable ? ":x" : "");
    case store::TreeEntry::Kind::Directory:
        return "d:" + e.digest.hash;
    case store::TreeEntry::Kind::Symlink:
        return "l:" + e.target;
    case store::TreeEntry::Kind::Absent:
        break;
    }
    return "";
}

std::string ListingAt(store::MerkleBuilder& merkle, const store::Digest& root, const std::string& path) {
    auto names = merkle.ListNames(root, path);
    if (!names) return "";
    std::string joined;
    for (const auto& n : *names) joined += n + "\n";
    return store::CasStore::ComputeDigest(joined).hash;
}

} // namespace

KeyMode ModeOf(const v1::Item& item) {
    if (!item.depfile().empty()) return KeyMode::Depfile;
    if (!item.inputs().empty()) return KeyMode::Inputs;
    return KeyMode::Whole;
}

std::string WorkspacePath(const std::string& cwd, const std::string& path) {
    std::filesystem::path p = path.empty() ? std::filesystem::path(cwd) : std::filesystem::path(cwd) / path;
    if (p.is_absolute()) throw std::invalid_argument("path must be relative: " + p.string());
    p = p.lexically_normal();
    std::string s = p.string();
    if (s == "." || s.empty()) return "";
    if (s == ".." || s.rfind("../", 0) == 0) throw std::invalid_argument("path leaves the workspace: " + path);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string WeakKey(const v1::Item& item, const v1::Stage& stage, const std::string& env_identity,
                    const std::string& arch) {
    KeyWriter w;
    w.Tag('V');
    w.Str("zm-item-1");
    w.Tag('A');
    for (const auto& a : item.argv()) w.Str(a);
    w.Tag('C');
    for (const auto& a : stage.combine().argv()) w.Str(a);
    w.Tag('E');
    std::map<std::string, std::string> env(item.env().begin(), item.env().end());
    for (const auto& [k, v] : env) {
        w.Str(k);
        w.Str(v);
    }
    w.Tag('W');
    w.Str(WorkspacePath("", item.cwd()));
    w.Tag('O');
    std::vector<std::string> outs;
    for (const auto& o : item.outputs()) outs.push_back(o.path() + (o.optional() ? "?" : ""));
    std::sort(outs.begin(), outs.end());
    for (const auto& o : outs) w.Str(o);
    w.Tag('X');
    w.Str(item.changed_dest().empty() ? "" : "changed");
    w.Tag('I');
    for (const auto& i : item.inputs()) w.Str(i);
    w.Tag('D');
    w.Str(item.depfile());
    for (const auto& s : item.search_dirs()) w.Str(s);
    w.Tag('S');
    w.Str(env_identity);
    w.Tag('R');
    w.Str(arch);
    w.Tag('N');
    w.Str(stage.network() ? "net" : "");
    return w.Hash();
}

v1::Footprint Observe(store::MerkleBuilder& merkle, const store::Digest& root, const v1::Item& item,
                      const std::vector<std::string>& footprint) {
    v1::Footprint fp;
    auto add_file = [&](const std::string& path) {
        auto* f = fp.add_files();
        f->set_path(path);
        f->set_hash(ValueAt(merkle, root, path));
    };
    switch (ModeOf(item)) {
    case KeyMode::Whole: {
        auto* f = fp.add_files();
        f->set_path("");
        f->set_hash("d:" + root.hash);
        break;
    }
    case KeyMode::Inputs: {
        std::vector<std::string> paths;
        for (const auto& i : item.inputs()) paths.push_back(WorkspacePath("", i));
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        for (const auto& p : paths) add_file(p);
        break;
    }
    case KeyMode::Depfile: {
        std::vector<std::string> files(footprint.begin(), footprint.end());
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        for (const auto& p : files) add_file(p);
        std::vector<std::string> dirs;
        const std::string cwd = WorkspacePath("", item.cwd());
        for (const auto& d : item.search_dirs()) dirs.push_back(WorkspacePath(cwd, d));
        std::sort(dirs.begin(), dirs.end());
        dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());
        for (const auto& d : dirs) {
            auto* l = fp.add_listings();
            l->set_path(d);
            l->set_hash(ListingAt(merkle, root, d));
        }
        break;
    }
    }
    return fp;
}

std::string FullKey(const std::string& weak, const v1::Footprint& observed) {
    KeyWriter w;
    w.Str(weak);
    w.Tag('F');
    for (const auto& f : observed.files()) {
        w.Str(f.path());
        w.Str(f.hash());
    }
    w.Tag('L');
    for (const auto& l : observed.listings()) {
        w.Str(l.path());
        w.Str(l.hash());
    }
    return w.Hash();
}

v1::Footprint Revalue(store::MerkleBuilder& merkle, const store::Digest& root, const v1::Footprint& recorded) {
    v1::Footprint now;
    for (const auto& f : recorded.files()) {
        auto* n = now.add_files();
        n->set_path(f.path());
        n->set_hash(f.path().empty() ? "d:" + root.hash : ValueAt(merkle, root, f.path()));
    }
    for (const auto& l : recorded.listings()) {
        auto* n = now.add_listings();
        n->set_path(l.path());
        n->set_hash(ListingAt(merkle, root, l.path()));
    }
    return now;
}

std::vector<std::string> Changed(store::MerkleBuilder& merkle, const v1::Footprint& before, const v1::Footprint& now,
                                 size_t limit) {
    std::vector<std::string> out;
    auto dir_of = [](const std::string& v) { return v.rfind("d:", 0) == 0 ? v.substr(2) : std::string(); };
    for (int i = 0; i < before.files_size() && i < now.files_size() && out.size() < limit; ++i) {
        const auto& a = before.files(i);
        const auto& b = now.files(i);
        if (a.hash() == b.hash()) continue;
        const std::string da = dir_of(a.hash()), db = dir_of(b.hash());
        if (!da.empty() && !db.empty()) {
            try {
                for (const auto& p : merkle.Diff({da, 0}, {db, 0}, limit - out.size())) {
                    out.push_back(a.path().empty() ? p : a.path() + "/" + p);
                }
                continue;
            } catch (const std::exception&) {
                // the old tree was collected: name the directory itself
            }
        }
        out.push_back(a.path().empty() ? "." : a.path());
    }
    for (int i = 0; i < before.listings_size() && i < now.listings_size() && out.size() < limit; ++i) {
        if (before.listings(i).hash() != now.listings(i).hash()) out.push_back(before.listings(i).path() + "/");
    }
    return out;
}

} // namespace zeromesh::run
