#pragma once

#include <filesystem>
#include <string>

namespace zeromesh::node {

// Paths and ports of one zmd instance (Global Constraints / spec §12).
struct DaemonConfig {
    std::filesystem::path state_dir;   // node id, CAS at cas/, scratch at scratch/
    std::filesystem::path runtime_dir; // zmd.sock
    std::filesystem::path config_dir;  // donor.toml (Task 17)
    int node_port{8981};               // NodeApi TCP port (Task 17)
    int discovery_port{8988};          // discovery multicast UDP port (Task 17)

    // Resolved from the environment:
    //   ZM_STATE_DIR      default ~/.local/share/zeromesh
    //   ZM_RUNTIME_DIR    default $XDG_RUNTIME_DIR/zeromesh, else /tmp/zeromesh-<uid>
    //   ZM_CONFIG_DIR     default ~/.config/zeromesh
    //   ZM_NODE_PORT      default 8981
    //   ZM_DISCOVERY_PORT default 8988
    static DaemonConfig FromEnv();

    [[nodiscard]] std::filesystem::path socket_path() const { return runtime_dir / "zmd.sock"; }
};

// True when something accepts connections on the unix socket at `path`.
bool UnixSocketAlive(const std::filesystem::path& path);

// The long-lived zmd process: ClientApi on an owner-only unix socket, local execution.
class Daemon {
public:
    explicit Daemon(DaemonConfig config);
    ~Daemon();

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    // Serves until SIGINT/SIGTERM or Shutdown(), then stops cleanly and removes the socket.
    // Returns the process exit code: 0 after a clean stop, 1 on startup failure (including
    // "zmd is already running" when a live daemon holds the socket).
    int Run();

    // Thread-safe and async-signal-safe: makes Run() return.
    void Shutdown();

    [[nodiscard]] const DaemonConfig& config() const { return config_; }

private:
    DaemonConfig config_;
    int wake_fd_{-1}; // eventfd written by Shutdown()
};

} // namespace zeromesh::node
