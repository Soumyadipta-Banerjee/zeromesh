#include "plugin/loader.hpp"

#include "plugin/client_api_handle.hpp"

#include <dlfcn.h>

#include <cstdlib>
#include <iostream>
#include <sstream>

#ifndef ZM_INSTALL_PREFIX
#define ZM_INSTALL_PREFIX "/usr/local"
#endif

namespace zeromesh::plugin {

Registry::~Registry() {
    // Plugin objects must die before their code is unmapped.
    StopFrontends();
    envs_.clear();
    dispatch_owned_.clear();
    dispatch_by_name_.clear();
    frontends_.clear();
    for (void* h : handles_) dlclose(h);
}

void Registry::RegisterEnv(std::unique_ptr<IEnvProvider> env) {
    if (!env || Env(env->Name()) != nullptr) return;
    envs_.push_back(std::move(env));
}

void Registry::RegisterDispatch(std::unique_ptr<IDispatchPolicy> policy) {
    if (!policy) return;
    const std::string name(policy->Name());
    if (dispatch_by_name_.contains(name)) return;
    dispatch_by_name_[name] = policy.get();
    dispatch_owned_.push_back(std::move(policy));
}

void Registry::RegisterFrontend(std::unique_ptr<IFrontend> frontend) {
    if (!frontend) return;
    for (const auto& f : frontends_) {
        if (f && f->Name() == frontend->Name()) return;
    }
    frontends_.push_back(std::move(frontend));
}

std::vector<IFrontend*> Registry::Frontends() const {
    std::vector<IFrontend*> out;
    out.reserve(frontends_.size());
    for (const auto& f : frontends_) {
        if (f) out.push_back(f.get());
    }
    return out;
}

void Registry::StartFrontends(ClientApiHandle& handle, const PluginConfig& config) {
    StopFrontends();
    for (auto& f : frontends_) {
        if (!f) continue;
        if (f->Start(handle, config)) {
            started_frontends_.push_back(f.get());
        } else {
            std::cerr << "zeromesh: frontend '" << f->Name() << "' failed to start\n";
        }
    }
}

void Registry::StopFrontends() {
    for (auto it = started_frontends_.rbegin(); it != started_frontends_.rend(); ++it) {
        if (*it) (*it)->Stop();
    }
    started_frontends_.clear();
}

bool Registry::LoadFile(const std::filesystem::path& so, std::string* error) {
    void* handle = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        if (error) *error = std::string("dlopen failed: ") + dlerror();
        return false;
    }
    using EntryFn = const PluginEntry* (*)();
    auto entry_fn = reinterpret_cast<EntryFn>(dlsym(handle, "zm_plugin_entry"));
    if (entry_fn == nullptr) {
        if (error) *error = so.string() + ": missing zm_plugin_entry symbol";
        dlclose(handle);
        return false;
    }
    const PluginEntry* entry = entry_fn();
    if (entry == nullptr || entry->abi_major != kAbiMajor) {
        if (error) {
            std::ostringstream ss;
            ss << so.string() << ": plugin abi " << (entry ? entry->abi_major : 0)
               << " does not match core abi " << kAbiMajor;
            *error = ss.str();
        }
        dlclose(handle);
        return false;
    }
    handles_.push_back(handle);
    if (entry->make_env != nullptr) RegisterEnv(std::unique_ptr<IEnvProvider>(entry->make_env()));
    if (entry->make_frontend != nullptr) frontends_.emplace_back(entry->make_frontend());
    if (entry->make_dispatch != nullptr) RegisterDispatch(std::unique_ptr<IDispatchPolicy>(entry->make_dispatch()));
    return true;
}

size_t Registry::LoadSearchPath() {
    std::vector<std::filesystem::path> dirs;
    if (const char* p = std::getenv("ZM_PLUGIN_PATH")) {
        std::stringstream ss(p);
        std::string item;
        while (std::getline(ss, item, ':')) {
            if (!item.empty()) dirs.emplace_back(item);
        }
    }
    if (const char* home = std::getenv("HOME")) {
        dirs.emplace_back(std::filesystem::path(home) / ".local/lib/zeromesh/plugins");
    }
    dirs.emplace_back(std::filesystem::path(ZM_INSTALL_PREFIX) / "lib/zeromesh/plugins");

    size_t loaded = 0;
    for (const auto& dir : dirs) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".so") continue;
            std::string err;
            if (LoadFile(e.path(), &err)) ++loaded;
        }
    }
    return loaded;
}

IEnvProvider* Registry::Env(std::string_view name) const {
    for (const auto& e : envs_) {
        if (e->Name() == name) return e.get();
    }
    return nullptr;
}

std::vector<std::string> Registry::EnvNames() const {
    std::vector<std::string> names;
    for (const auto& e : envs_) names.emplace_back(e->Name());
    return names;
}

IDispatchPolicy* Registry::Dispatch(std::string_view name) const {
    auto it = dispatch_by_name_.find(std::string(name));
    return it == dispatch_by_name_.end() ? nullptr : it->second;
}

} // namespace zeromesh::plugin
