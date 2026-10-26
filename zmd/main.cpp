// zmd: the ZeroMesh daemon. Serves zeromesh.v1.ClientApi on $XDG_RUNTIME_DIR/zeromesh/zmd.sock
// and runs tasks locally. Stop it with SIGINT or SIGTERM.

#include "node/daemon.hpp"

#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "version") {
            std::cout << "zmd (ZeroMesh) 0.1.0\n";
            return 0;
        }
        std::cout << "Usage: zmd\n\n"
                  << "Runs the ZeroMesh daemon in the foreground. Environment overrides:\n"
                  << "  ZM_STATE_DIR    state (node id, CAS)   default ~/.local/share/zeromesh\n"
                  << "  ZM_RUNTIME_DIR  socket directory       default $XDG_RUNTIME_DIR/zeromesh\n"
                  << "  ZM_CONFIG_DIR   configuration          default ~/.config/zeromesh\n";
        return (a == "--help" || a == "-h") ? 0 : 1;
    }
    try {
        zeromesh::node::Daemon daemon(zeromesh::node::DaemonConfig::FromEnv());
        return daemon.Run();
    } catch (const std::exception& e) {
        std::cerr << "zmd: " << e.what() << "\n";
        return 1;
    }
}
