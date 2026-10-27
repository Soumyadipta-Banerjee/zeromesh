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

inline constexpr uint32_t kAbiMajor = 3;

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

struct DispatchTaskView {
    std::string id;
    std::string name;
    int cores{1};
    int64_t mem_bytes{0};
    int attempt{0};
    int64_t age_ms{0};
    bool preemptible{true};
    bool may_run_local{true};
    bool may_run_remote{true};
    std::string arch;                       // resolved; never empty when the requester knows its arch
    int64_t input_bytes{0};                 // size of the task's input tree
    std::vector<std::string> exclude_nodes; // failed or lacking the command; never assign there
};

struct DispatchNodeView {
    std::string node_id;
    bool is_local{false};
    int slots{0};        // 0: node predates slots; capacity is cores_total / task cores
    int queue_depth{0};
    int in_flight{0};    // tasks of every job on this requester already reserved there
    int cores_total{0};
    uint64_t mem_available{0};
    double cpu_load_pct{0.0};
    bool on_ac{true};
    int64_t latency_ms{0};
    int64_t missing_bytes{-1};  // of the job's input tree; -1 unknown
    std::string arch;
};

struct DispatchAssignment {
    std::string task_id;
    std::string node_id;
};

// Chooses (task, node) pairs for a job's pending queue. Plain C++ only in the ABI.
// Called whenever a slot frees, a task is queued or the mesh changes. The core drops
// assignments to excluded or ineligible nodes and anything past a node's slots +
// queue_depth; donors refuse the rest. Returning nothing leaves the tasks queued.
class IDispatchPolicy {
public:
    virtual ~IDispatchPolicy() = default;
    virtual std::string_view Name() const = 0;
    virtual std::vector<DispatchAssignment> Decide(const std::vector<DispatchTaskView>& pending,
                                                   const std::vector<DispatchNodeView>& nodes,
                                                   const std::map<std::string, std::string>& params,
                                                   int job_in_flight) = 0;
    virtual void OnResult(const std::string& /*task_id*/, const std::string& /*node_id*/, bool /*ok*/,
                          int64_t /*duration_ms*/) {}
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
    IEnvProvider* (*make_env)();        // may be null
    IFrontend* (*make_frontend)();      // may be null
    IDispatchPolicy* (*make_dispatch)(); // may be null
};

} // namespace zeromesh::plugin

extern "C" const zeromesh::plugin::PluginEntry* zm_plugin_entry();
