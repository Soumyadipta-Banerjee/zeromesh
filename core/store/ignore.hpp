#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::store {

// A subset of gitignore syntax, read from <root>/.zmignore: comments, blank lines,
// "!negation", trailing "/" (directories only), leading "/" or an inner "/" (anchored to
// the root), "*", "?" and "**". The last matching rule wins. ".git" is always ignored.
// A path inside an ignored directory is ignored.
class IgnoreRules {
public:
    static IgnoreRules ForRoot(const std::filesystem::path& root);
    static IgnoreRules None();
    static IgnoreRules FromLines(const std::vector<std::string>& lines);

    // rel_path uses '/' separators and is relative to the root.
    [[nodiscard]] bool Ignored(std::string_view rel_path, bool is_dir) const;

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
};

} // namespace zeromesh::store
