#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zeromesh::store {

// A subset of gitignore syntax, read from <root>/.zmignore: comments, blank lines,
// "!negation", trailing "/" (directories only), leading "/" or an inner "/" (anchored to
// the root), "*", "?" and "**". The last matching rule wins. ".git" is always ignored.
// A path inside an ignored directory is ignored.
class IgnoreRules {
public:
    // `extra` lines (an adapter's excludes) come before <root>/.zmignore, so the user's file can
    // still re-include ("!pattern") what they exclude.
    static IgnoreRules ForRoot(const std::filesystem::path& root, const std::vector<std::string>& extra = {});
    static IgnoreRules None();
    static IgnoreRules FromLines(const std::vector<std::string>& lines);

    // rel_path uses '/' separators and is relative to the root.
    [[nodiscard]] bool Ignored(std::string_view rel_path, bool is_dir) const;

    // The same for a walk that never enters an ignored directory: rel_path's directories have
    // passed already, so only rel_path itself is matched.
    [[nodiscard]] bool IgnoredEntry(std::string_view rel_path, bool is_dir) const;

private:
    struct Rule {
        std::vector<std::string> segments; // pattern split on '/'
        bool negate{false};
        bool dir_only{false};
        bool anchored{false};
    };

    void Add(std::string line);
    [[nodiscard]] bool MatchesSelf(const std::vector<std::string>& path, bool is_dir) const;

    std::vector<Rule> rules_;
    // Rules without a wildcard are looked up instead of tried in turn, so a long list of exact
    // paths (an adapter's excludes) costs one lookup per entry: anchored ones by the path they
    // name, the others by the entry's name. Each maps to its rules' indices in rules_, ascending.
    std::unordered_map<std::string, std::vector<size_t>> exact_paths_;
    std::unordered_map<std::string, std::vector<size_t>> exact_names_;
    std::vector<size_t> patterns_; // the other rules' indices, ascending
};

} // namespace zeromesh::store
