// zmd: the ZeroMesh daemon. Serves zeromesh.v1.ClientApi on $XDG_RUNTIME_DIR/zeromesh/zmd.sock
// and runs tasks locally. Stop it with SIGINT or SIGTERM.

#include "node/daemon.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

namespace {

bool ParseIntArg(const char* s, int* out) {
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    *out = static_cast<int>(v);
    return true;
}

void PrintUsage() {
    std::cout << "Usage: zmd [options]\n\n"
              << "Runs the ZeroMesh daemon in the foreground.\n\n"
              << "Options:\n"
              << "  --slots N              worker slots (tasks at once), default 1\n"
              << "  --cores-per-slot N     cores per slot (0 = share all donated cores)\n"
              << "  --queue-depth N        extra queued tasks beyond slots, default 0\n"
              << "  --max-per-requester N  cap tasks per requester (0 = unlimited)\n"
              << "  --help, -h             show this help\n"
              << "  --version              show version\n\n"
              << "Environment (overrides donor.toml when set):\n"
              << "  ZM_STATE_DIR, ZM_RUNTIME_DIR, ZM_CONFIG_DIR\n"
              << "  ZM_NODE_PORT, ZM_DISCOVERY_PORT\n"
              << "  ZM_SLOTS, ZM_CORES_PER_SLOT, ZM_QUEUE_DEPTH, ZM_MAX_PER_REQUESTER\n";
}

} // namespace

int main(int argc, char** argv) {
    zeromesh::node::DaemonConfig config = zeromesh::node::DaemonConfig::FromEnv();
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "version") {
            std::cout << "zmd (ZeroMesh) 0.1.0\n";
            return 0;
        }
        if (a == "--help" || a == "-h") {
            PrintUsage();
            return 0;
        }
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "zmd: " << flag << " requires a value\n";
                std::exit(1);
            }
            return argv[++i];
        };
        int v = 0;
        if (a == "--slots") {
            if (!ParseIntArg(need("--slots"), &v) || v < 1) {
                std::cerr << "zmd: invalid --slots\n";
                return 1;
            }
            config.slots = v;
        } else if (a == "--cores-per-slot") {
            if (!ParseIntArg(need("--cores-per-slot"), &v) || v < 0) {
                std::cerr << "zmd: invalid --cores-per-slot\n";
                return 1;
            }
            config.cores_per_slot = v;
        } else if (a == "--queue-depth") {
            if (!ParseIntArg(need("--queue-depth"), &v) || v < 0) {
                std::cerr << "zmd: invalid --queue-depth\n";
                return 1;
            }
            config.queue_depth = v;
        } else if (a == "--max-per-requester") {
            if (!ParseIntArg(need("--max-per-requester"), &v) || v < 0) {
                std::cerr << "zmd: invalid --max-per-requester\n";
                return 1;
            }
            config.max_per_requester = v;
        } else {
            PrintUsage();
            return 1;
        }
    }
    try {
        zeromesh::node::Daemon daemon(std::move(config));
        return daemon.Run();
    } catch (const std::exception& e) {
        std::cerr << "zmd: " << e.what() << "\n";
        return 1;
    }
}
