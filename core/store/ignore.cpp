#include "store/ignore.hpp"

#include <fnmatch.h>

#include <algorithm>
#include <fstream>
#include <optional>

namespace zeromesh::store {

namespace {

std::vector<std::string> Split(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t slash = s.find('/', start);
        if (slash == std::string_view::npos) slash = s.size();
        if (slash > start) out.emplace_back(s.substr(start, slash - start));
        start = slash + 1;
    }
    return out;
}

// Segment-wise glob: "**" matches zero or more whole segments.
bool GlobSegments(const std::vector<std::string>& pat, size_t pi, const std::vector<std::string>& path, size_t si) {
    if (pi == pat.size()) return si == path.size();
    if (pat[pi] == "**") {
        for (size_t k = si; k <= path.size(); ++k) {
            if (GlobSegments(pat, pi + 1, path, k)) return true;
        }
        return false;
    }
    if (si == path.size()) return false;
    if (fnmatch(pat[pi].c_str(), path[si].c_str(), FNM_PERIOD) != 0) return false;
    return GlobSegments(pat, pi + 1, path, si + 1);
}

// No wildcard and no escape: fnmatch would match the segment by equality alone.
bool Literal(const std::string& segment) {
    return segment.find_first_of("*?[\\") == std::string::npos;
}

std::string Join(const std::vector<std::string>& path) {
    std::string out;
    for (const auto& s : path) {
        if (!out.empty()) out += '/';
        out += s;
    }
    return out;
}

} // namespace

IgnoreRules IgnoreRules::None() {
    return IgnoreRules{};
}

IgnoreRules IgnoreRules::FromLines(const std::vector<std::string>& lines) {
    IgnoreRules r;
    r.Add(".git");
    for (const auto& line : lines) r.Add(line);
    return r;
}

IgnoreRules IgnoreRules::ForRoot(const std::filesystem::path& root, const std::vector<std::string>& extra) {
    IgnoreRules r;
    r.Add(".git");
    for (const auto& line : extra) r.Add(line);
    std::ifstream in(root / ".zmignore");
    std::string line;
    while (std::getline(in, line)) r.Add(line);
    return r;
}

void IgnoreRules::Add(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.empty() || line[0] == '#') return;
    Rule rule;
    if (line[0] == '!') {
        rule.negate = true;
        line.erase(0, 1);
    }
    if (!line.empty() && line.back() == '/') {
        rule.dir_only = true;
        line.pop_back();
    }
    if (!line.empty() && line[0] == '/') {
        rule.anchored = true;
        line.erase(0, 1);
    }
    if (line.find('/') != std::string::npos) rule.anchored = true;
    rule.segments = Split(line);
    if (rule.segments.empty()) return;
    const size_t index = rules_.size();
    if (!std::ranges::all_of(rule.segments, Literal)) {
        patterns_.push_back(index);
    } else if (rule.anchored) {
        exact_paths_[Join(rule.segments)].push_back(index);
    } else {
        exact_names_[rule.segments[0]].push_back(index);
    }
    rules_.push_back(std::move(rule));
}

bool IgnoreRules::MatchesSelf(const std::vector<std::string>& path, bool is_dir) const {
    // The last matching rule decides: the latest pattern that matches, or a later exact rule.
    std::optional<size_t> last;
    for (auto i = patterns_.rbegin(); i != patterns_.rend(); ++i) {
        const Rule& rule = rules_[*i];
        if (rule.dir_only && !is_dir) continue;
        const bool match = rule.anchored ? GlobSegments(rule.segments, 0, path, 0)
                                         : fnmatch(rule.segments[0].c_str(), path.back().c_str(), FNM_PERIOD) == 0;
        if (match) {
            last = *i;
            break;
        }
    }
    auto exact = [&](const std::unordered_map<std::string, std::vector<size_t>>& rules, const std::string& key) {
        const auto found = rules.find(key);
        if (found == rules.end()) return;
        for (auto i = found->second.rbegin(); i != found->second.rend() && (!last || *i > *last); ++i) {
            if (rules_[*i].dir_only && !is_dir) continue;
            last = *i;
            return;
        }
    };
    exact(exact_names_, path.back());
    if (!exact_paths_.empty()) exact(exact_paths_, Join(path));
    return last && !rules_[*last].negate;
}

bool IgnoreRules::Ignored(std::string_view rel_path, bool is_dir) const {
    auto path = Split(rel_path);
    if (path.empty()) return false;
    // A path inside an ignored directory is ignored, whatever later rules say.
    std::vector<std::string> prefix;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        prefix.push_back(path[i]);
        if (MatchesSelf(prefix, true)) return true;
    }
    return MatchesSelf(path, is_dir);
}

bool IgnoreRules::IgnoredEntry(std::string_view rel_path, bool is_dir) const {
    const auto path = Split(rel_path);
    return !path.empty() && MatchesSelf(path, is_dir);
}

} // namespace zeromesh::store
