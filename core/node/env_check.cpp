#include "node/env_check.hpp"

#include "exec/toolchain_digest.hpp"
#include "plugins/env-host/env_host.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <stdexcept>

namespace zeromesh::node {

void StampPrograms(v1::Env& env) {
    if (env.has_tree()) return;
    for (auto& p : *env.mutable_programs()) {
        if (!p.version().empty() || !p.digest().empty()) continue;
        auto path = envhost::Resolve(p.name());
        if (!path) throw std::invalid_argument("program '" + p.name() + "' is not installed on this node");
        const std::string digest = exec::ToolchainDigest(*path, "");
        if (digest.empty()) throw std::invalid_argument("cannot read program '" + *path + "'");
        p.set_digest(digest);
    }
}

std::string EnvIdentity(const v1::Env& env) {
    std::string id;
    if (env.has_tree()) {
        id = "tree:" + env.tree().hash();
    } else {
        std::vector<std::string> parts;
        for (const auto& p : env.programs()) {
            parts.push_back(p.name() + (p.version().empty() ? "@" + p.digest() : "~" + p.version()));
        }
        std::sort(parts.begin(), parts.end());
        id = "host";
        for (const auto& s : parts) id += ";" + s;
    }
    std::vector<std::string> shared(env.shared_paths().begin(), env.shared_paths().end());
    std::sort(shared.begin(), shared.end());
    for (const auto& s : shared) id += "|shared:" + s;
    std::map<std::string, std::string> vars(env.vars().begin(), env.vars().end());
    for (const auto& [k, v] : vars) id += "|" + k + "=" + v;
    return id;
}

std::optional<Version> ParseVersion(std::string_view text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        Version v;
        size_t j = i;
        while (j < text.size()) {
            size_t k = j;
            while (k < text.size() && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
            if (k == j) break;
            v.parts.push_back(std::stol(std::string(text.substr(j, k - j))));
            if (k + 1 < text.size() && text[k] == '.' && std::isdigit(static_cast<unsigned char>(text[k + 1]))) {
                j = k + 1;
            } else {
                break;
            }
        }
        return v;
    }
    return std::nullopt;
}

std::optional<bool> Satisfies(const Version& version, std::string_view constraint) {
    // Compare with missing parts as zero, so "4.2" == "4.2.0".
    auto cmp = [](Version a, Version b) {
        const size_t n = std::max(a.parts.size(), b.parts.size());
        a.parts.resize(n, 0);
        b.parts.resize(n, 0);
        return a <=> b;
    };
    bool any = false;
    size_t start = 0;
    while (start <= constraint.size()) {
        size_t comma = constraint.find(',', start);
        if (comma == std::string_view::npos) comma = constraint.size();
        std::string clause(constraint.substr(start, comma - start));
        start = comma + 1;
        clause.erase(std::remove_if(clause.begin(), clause.end(), ::isspace), clause.end());
        if (clause.empty()) continue;
        std::string op = "=="; // a bare version means exactly that version
        size_t skip = 0;
        for (const char* o : {">=", "<=", "==", "!=", ">", "<"}) {
            if (clause.rfind(o, 0) == 0) {
                op = o;
                skip = op.size();
                break;
            }
        }
        auto want = ParseVersion(std::string_view(clause).substr(skip));
        if (!want) return std::nullopt;
        const auto c = cmp(version, *want);
        bool ok = false;
        if (op == ">=") ok = c >= 0;
        if (op == "<=") ok = c <= 0;
        if (op == ">") ok = c > 0;
        if (op == "<") ok = c < 0;
        if (op == "==") ok = c == 0;
        if (op == "!=") ok = c != 0;
        if (!ok) return false;
        any = true;
    }
    if (!any) return std::nullopt;
    return true;
}

std::optional<EnvRefusal> CheckEnv(const v1::Env& env, const VersionProbe& probe) {
    for (const auto& path : env.shared_paths()) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            return EnvRefusal{v1::REJECT_ENV, "shared path " + path + " is not on this node"};
        }
    }
    if (env.has_tree()) return std::nullopt; // the tree brings its own programs
    for (const auto& p : env.programs()) {
        auto path = envhost::Resolve(p.name());
        if (!path) return EnvRefusal{v1::REJECT_ENV, "'" + p.name() + "' is not installed"};
        if (p.version().empty()) {
            if (!p.digest().empty() && exec::ToolchainDigest(*path, "") != p.digest()) {
                return EnvRefusal{v1::REJECT_TOOLCHAIN, "'" + p.name() + "' is not the requester's exact build"};
            }
            continue;
        }
        auto printed = probe ? probe(p, *path) : std::nullopt;
        auto have = printed ? ParseVersion(*printed) : std::nullopt;
        if (!have) return EnvRefusal{v1::REJECT_ENV, "cannot tell which version of '" + p.name() + "' is installed"};
        auto ok = Satisfies(*have, p.version());
        if (!ok) return EnvRefusal{v1::REJECT_ENV, "bad version constraint '" + p.version() + "'"};
        if (!*ok) {
            std::string v;
            for (size_t i = 0; i < have->parts.size(); ++i) v += (i ? "." : "") + std::to_string(have->parts[i]);
            return EnvRefusal{v1::REJECT_ENV, "'" + p.name() + "' " + v + " does not satisfy " + p.version()};
        }
    }
    return std::nullopt;
}

} // namespace zeromesh::node
