#include "discovery/peer_table.hpp"
#include "executor/host_guard.hpp"
#include "ui/tui.hpp"

#include <iostream>
#include <memory>
#include <string>

namespace {

void PrintUsage(const char* prog) {
    std::cout << "ZeroMesh v0.1.0 (restructuring: see PLAN.md)\n\n"
              << "Usage: " << prog << " <command>\n\n"
              << "Commands:\n"
              << "  status   Show this node and discovered peers\n"
              << "  version  Show version\n"
              << std::endl;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 0;
    }

    std::string command = argv[1];

    if (command == "version" || command == "--version" || command == "-v") {
        std::cout << "ZeroMesh v0.1.0\n";
        return 0;
    }

    if (command == "status") {
        auto peer_table = std::make_shared<zeromesh::discovery::PeerTable>();
        zeromesh::ui::TerminalUI tui(peer_table);
        tui.PrintStatusSnapshot();
        return 0;
    }

    PrintUsage(argv[0]);
    return 1;
}
