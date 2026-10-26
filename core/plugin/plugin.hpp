#pragma once

// ZeroMesh core plugin ABI (spec §6.2). Plugins run inside zmd and are trusted.
// A plugin shared library exports one C symbol, zm_plugin_entry(), returning a
// PluginEntry. The core refuses entries whose abi_major differs from kAbiMajor.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zeromesh::plugin {

inline constexpr uint32_t kAbiMajor = 2;

struct Bind {
    std::string host;
    std::string sandbox;
    bool read_only{true};
};

// Becomes `bwrap --symlink <target> <link>`.
struct Symlink {
    std::string target;
    std::string link;
};

// What an environment provider asks the sandbox to apply for one task.
struct SandboxEnv {
    std::vector<Bind> binds;
    std::vector<Symlink> symlinks;
    std::vector<std::pair<std::string, std::string>> env;
};

struct EnvParams {
    std::map<std::string, std::string> params;
    std::vector<std::string> argv;
};

struct Availability {
    bool ok{false};
    std::string reason;
};

// Provides blob data to plugins that need it (e.g. env-mirror fetches the
// requester's file chunks). The core supplies an implementation backed by
// NodeClient::PullBlobs; plugins may define stubs for testing.
class IBlobSource {
public:
    virtual ~IBlobSource() = default;
    /// Fetches a blob by its SHA-256 hex hash. Returns nullopt on failure.
    virtual std::optional<std::string> FetchBlob(const std::string& hash) = 0;
};

struct TaskContext {
    std::string task_id;
    EnvParams env;
    std::filesystem::path scratch_dir;
    /// Node id of the machine that submitted the task (empty for local tasks).
    std::string requester_node_id;
    /// Blob source connected to the requester; null for local / host-env tasks.
    IBlobSource* blob_source{nullptr};
};

template <class T>
struct Result {
    std::optional<T> value;
    std::string error;
};

// Prepares where and how a task's software comes from on the executing node.
class IEnvProvider {
public:
    virtual ~IEnvProvider() = default;
    virtual std::string_view Name() const = 0;
    // Can this node run tasks with these env params? Called while deciding an offer.
    virtual Availability Check(const EnvParams&) = 0;
    virtual Result<SandboxEnv> Prepare(const TaskContext&) = 0;
    virtual void Release(const TaskContext&) = 0;
};

// Concrete type lives in plugin/client_api_handle.hpp (needs generated gRPC headers).
class ClientApiHandle;

struct PluginConfig {
    std::map<std::string, std::string> values;
};

// Exposes another protocol or UI on top of the ClientApi (e.g. REAPI).
class IFrontend {
public:
    virtual ~IFrontend() = default;
    virtual std::string_view Name() const = 0;
    virtual bool Start(ClientApiHandle&, const PluginConfig&) = 0;
    virtual void Stop() = 0;
};

struct PluginEntry {
    uint32_t abi_major;
    const char* name;
    IEnvProvider* (*make_env)();   // may be null
    IFrontend* (*make_frontend)(); // may be null
};

} // namespace zeromesh::plugin

extern "C" const zeromesh::plugin::PluginEntry* zm_plugin_entry();
