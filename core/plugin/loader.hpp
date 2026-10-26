#pragma once

#include "plugin/plugin.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::plugin {

// Holds built-in and dlopen'ed plugins for the daemon's lifetime.
// On duplicate names the first registration wins.
class Registry {
public:
    Registry() = default;
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    void RegisterEnv(std::unique_ptr<IEnvProvider> env);
    void RegisterFrontend(std::unique_ptr<IFrontend> frontend);

    // dlopen + ABI check. On failure returns false and explains in *error.
    bool LoadFile(const std::filesystem::path& so, std::string* error);

    // Loads every *.so from $ZM_PLUGIN_PATH (':'-separated),
    // ~/.local/lib/zeromesh/plugins and <prefix>/lib/zeromesh/plugins.
    size_t LoadSearchPath();

    [[nodiscard]] IEnvProvider* Env(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> EnvNames() const;

    // Front-ends loaded from shared plugins (and any registered built-ins).
    [[nodiscard]] std::vector<IFrontend*> Frontends() const;
    [[nodiscard]] IFrontend* Frontend(std::string_view name) const;

private:
    std::vector<std::unique_ptr<IEnvProvider>> envs_;
    std::vector<std::unique_ptr<IFrontend>> frontends_;
    std::vector<void*> handles_;
};

} // namespace zeromesh::plugin
