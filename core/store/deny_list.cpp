#include "store/deny_list.hpp"

#include <cstdlib>
#include <fstream>

namespace zeromesh::store {

const std::vector<std::string>& DefaultDenyPatterns() {
    // Unanchored names match at any depth, so ".ssh" also covers "home/me/.ssh".
    static const std::vector<std::string> kPatterns = {
        ".ssh",
        ".gnupg",
        ".password-store",
        ".aws",
        ".kube",
        ".docker",
        ".netrc",
        ".git-credentials",
        ".env*", // .env, .env.local, .envrc
        ".bash_history",
        ".zsh_history",
        ".mozilla",
        "**/.config/google-chrome",
        "**/.config/chromium",
        "**/.config/gh",
        "**/.config/zeromesh",
        "**/.local/share/zeromesh",
        "**/.local/share/keyrings",
        "**/.local/share/fish",
    };
    return kPatterns;
}

std::filesystem::path DefaultDenyFile() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::filesystem::path(home) / ".config" / "zeromesh" / "deny";
}

IgnoreRules DenyRules(const std::filesystem::path& user_file) {
    std::vector<std::string> lines = DefaultDenyPatterns();
    if (!user_file.empty()) {
        std::ifstream in(user_file);
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
    }
    return IgnoreRules::FromLines(lines);
}

} // namespace zeromesh::store
