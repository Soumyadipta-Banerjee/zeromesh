// zm: the ZeroMesh command-line client. A thin zeromesh.v1.ClientApi client of the local zmd.

#include "cli/client.hpp"
#include "cli/run_cmd.hpp"
#include "cli/status_cmd.hpp"
#include "cli/team_cmd.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace cli = zeromesh::cli;

void PrintUsage(std::ostream& os) {
    os << "ZeroMesh 0.1.0\n\n"
       << "Usage: zm [--socket PATH] <command> [args]\n\n"
       << "Commands:\n"
       << "  run [opts] -- CMD...  Run CMD on the mesh with the current directory as input\n"
       << "  status                Show this node and its peers\n"
       << "  monitor               Peers + tasks (WORKING/DONE; q / Ctrl-C)\n"
       << "  team create|join|leave|info\n"
       << "                        Create or join a team (quote the join phrase)\n"
       << "  version               Show the version\n\n"
       << "Global options:\n"
       << "  --socket PATH         zmd socket (default: $XDG_RUNTIME_DIR/zeromesh/zmd.sock)\n\n"
       << cli::kRunUsage;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::filesystem::path socket;
    size_t i = 0;
    for (; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--socket") {
            if (i + 1 >= args.size()) {
                std::cerr << "zm: --socket needs a path\n";
                return 1;
            }
            socket = args[++i];
        } else if (a.rfind("--socket=", 0) == 0) {
            socket = a.substr(9);
        } else if (a == "-h" || a == "--help") {
            PrintUsage(std::cout);
            return 0;
        } else if (a == "-v" || a == "--version") {
            std::cout << "ZeroMesh 0.1.0\n";
            return 0;
        } else {
            break;
        }
    }
    if (i >= args.size()) {
        PrintUsage(std::cout);
        return 0;
    }
    const std::string cmd = args[i];
    const std::vector<std::string> rest(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());

    if (cmd == "version") {
        std::cout << "ZeroMesh 0.1.0\n";
        return 0;
    }
    if (cmd == "help") {
        PrintUsage(std::cout);
        return 0;
    }
    if (cmd != "run" && cmd != "status" && cmd != "monitor" && cmd != "team") {
        std::cerr << "zm: unknown command '" << cmd << "'\n\n";
        PrintUsage(std::cerr);
        return 1;
    }

    // Parse before connecting, so usage errors don't depend on the daemon.
    std::optional<cli::RunOptions> run_opts;
    if (cmd == "run") {
        std::string error;
        run_opts = cli::ParseRunArgs(rest, &error);
        if (!run_opts) {
            std::cerr << "zm run: " << error << "\n\n" << cli::kRunUsage;
            return 1;
        }
    } else if (cmd == "team") {
        // Subcommands parse their own args inside TeamCommand.
    } else if (!rest.empty()) {
        std::cerr << "zm " << cmd << ": unexpected argument '" << rest.front() << "'\n";
        return 1;
    }

    if (socket.empty()) socket = cli::DefaultSocketPath();
    auto client = cli::Client::Connect(socket);
    if (!client) {
        std::cerr << cli::kNotRunning << "\n";
        return cli::kExitNotRunning;
    }

    if (cmd == "run") return cli::RunCommand(*client, *run_opts);
    if (cmd == "status") return cli::StatusCommand(*client);
    if (cmd == "team") return cli::TeamCommand(*client, rest);
    return cli::MonitorCommand(*client);
}
