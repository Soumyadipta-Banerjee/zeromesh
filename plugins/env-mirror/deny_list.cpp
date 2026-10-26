#include "plugins/env-mirror/deny_list.hpp"

#include <cstdlib>
#include <fstream>

namespace zeromesh::mirror {

namespace {

std::filesystem::path DefaultUserConfigFile() {
    const char* home = std::getenv("HOME");
    if (!home) return {};
    return std::filesystem::path(home) / ".config" / "zeromesh" / "mirror_deny";
}

} // namespace

const std::vector<std::string>& DenyList::DefaultPatterns() {
    static const std::vector<std::string> kDefaults = {
        ".ssh",
        "**/.ssh",
        ".gnupg",
        "**/.gnupg",
        ".config/zeromesh",
        "**/.config/zeromesh",
        ".local/share/zeromesh",
        "**/.local/share/zeromesh",
        ".local/share/keyrings",
        "**/.local/share/keyrings",
        ".env*",
        "**/.env*",
        ".mozilla",
        "**/.mozilla",
        ".config/google-chrome",
        "**/.config/google-chrome",
        ".config/chromium",
        "**/.config/chromium",
    };
    return kDefaults;
}

store::IgnoreRules DenyList::CreateIgnoreRules(const std::filesystem::path& custom_config_file) {
    std::vector<std::string> patterns = DefaultPatterns();

    std::filesystem::path config_file = custom_config_file;
    if (config_file.empty()) {
        config_file = DefaultUserConfigFile();
    }

    if (!config_file.empty() && std::filesystem::exists(config_file)) {
        std::ifstream in(config_file);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty() && line[0] != '#') {
                patterns.push_back(line);
            }
        }
    }

    return store::IgnoreRules::FromLines(patterns);
}

bool DenyList::IsDenied(std::string_view rel_path, bool is_dir) {
    static const auto kRules = CreateIgnoreRules();
    return kRules.Ignored(rel_path, is_dir);
}

} // namespace zeromesh::mirror
