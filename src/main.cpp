#include "cas/merkle.hpp"
#include "cas/store.hpp"
#include "discovery/mdns.hpp"
#include "discovery/peer_table.hpp"
#include "executor/host_guard.hpp"
#include "executor/runner.hpp"
#include "reapi/action_cache.hpp"
#include "reapi/cas_service.hpp"
#include "reapi/execution_service.hpp"
#include "reapi/server.hpp"
#include "scheduler/heuristics.hpp"
#include "scheduler/scheduler.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

void PrintBanner() {
    std::cout << R"(
  ______               __  __           _     
 |___  /              |  \/  |         | |    
    / / ___ _ __ ___  | \  / | ___  ___| |__  
   / / / _ \ '__/ _ \ | |\/| |/ _ \/ __| '_ \
  / /_|  __/ | | (_) || |  | |  __/\__ \ | | |
 /_____\___|_|  \___/ |_|  |_|\___||___/_| |_|
  Zero-Config P2P Remote Execution Mesh
)" << std::endl;
}

void PrintUsage(const char* prog) {
    PrintBanner();
    std::cout << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  up       Start the ZeroMesh background daemon (zmd) on 127.0.0.1:8980\n"
              << "  run      Execute a command via the local REAPI cluster\n"
              << "  status   Display active cluster peers and hardware telemetry\n"
              << "  monitor  Open the live interactive terminal dashboard\n"
              << "  version  Show version and kernel Landlock / bwrap capability\n"
              << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 0;
    }

    std::string command = argv[1];

    if (command == "version" || command == "--version" || command == "-v") {
        PrintBanner();
        std::cout << "ZeroMesh v0.1.0 (REAPI v2 compatible)\n"
                  << "C++ Standard: C++20\n"
                  << "Host CPU Load: " << zeromesh::executor::HostGuard::GetLocalCpuLoadPct() << "%\n"
                  << "Battery: " << zeromesh::executor::HostGuard::GetBatteryPercent() << "%\n"
                  << "AC Power: " << (zeromesh::executor::HostGuard::IsAcConnected() ? "Connected" : "Disconnected") << "\n"
                  << std::endl;
        return 0;
    }

    // Core subsystem initialization
    auto cas_store = std::make_shared<zeromesh::cas::CasStore>();
    auto merkle = std::make_shared<zeromesh::cas::MerkleBuilder>(cas_store);
    auto task_runner = std::make_shared<zeromesh::executor::TaskRunner>(cas_store, merkle);
    auto peer_table = std::make_shared<zeromesh::discovery::PeerTable>();
    auto scheduler = std::make_shared<zeromesh::scheduler::Scheduler>(peer_table);

    // mDNS zero-configuration peer discovery
    zeromesh::discovery::MdnsConfig mdns_cfg{
        .grpc_port = 8980
    };
    auto mdns_engine = std::make_shared<zeromesh::discovery::MdnsEngine>(peer_table, mdns_cfg);

    // Option 1: REAPI Gateway Services
    auto cas_svc = std::make_shared<zeromesh::reapi::CasService>(cas_store);
    auto ac_svc = std::make_shared<zeromesh::reapi::ActionCacheService>(cas_store);
    auto exec_svc = std::make_shared<zeromesh::reapi::ExecutionService>(cas_store, ac_svc, scheduler, task_runner);

    zeromesh::reapi::ServerConfig config{
        .host = "127.0.0.1",
        .port = 8980
    };
    zeromesh::reapi::ReapiServer server(config, cas_svc, ac_svc, exec_svc);

    if (command == "up" || command == "daemon") {
        PrintBanner();
        std::cout << "[+] ContentAddressableStorage (CAS) initialized at " << cas_store->GetBasePath() << "\n"
                  << "[+] ActionCache (AC) initialized with Cache-First fast path\n"
                  << "[+] Locality-Aware Scheduler initialized\n"
                  << "[+] mDNS Zero-Config Discovery listening on LAN multicast\n"
                  << "[+] REAPI v2 Gateway listening on 127.0.0.1:8980\n"
                  << "\nBuild tools can now connect:\n"
                  << "  export RECC_SERVER=127.0.0.1:8980\n"
                  << "  export CMAKE_CXX_COMPILER_LAUNCHER=recc\n"
                  << std::endl;

        mdns_engine->Start();
        server.Start();
        return 0;
    }

    if (command == "status") {
        PrintBanner();
        std::cout << "--- Local ZeroMesh Cluster Status ---\n"
                  << "Daemon Endpoint: 127.0.0.1:8980\n"
                  << "Protocol: Google REAPI v2 over gRPC\n"
                  << "CAS Storage: " << cas_store->GetBasePath() << "\n"
                  << "Active Peers: " << peer_table->Count() << "\n"
                  << "Scheduler Total Scheduled: " << scheduler->GetTotalScheduled() << "\n"
                  << "Scheduler Total Local:     " << scheduler->GetTotalLocal() << "\n"
                  << "Scheduler Total Offloaded: " << scheduler->GetTotalOffloaded() << "\n"
                  << std::endl;
        return 0;
    }

    if (command == "run") {
        if (argc < 3) {
            std::cerr << "Error: 'zm run' requires a command to execute (e.g. 'zm run -- g++ -c main.cpp')\n";
            return 1;
        }

        std::vector<std::string> args;
        for (int i = 2; i < argc; ++i) {
            args.push_back(argv[i]);
        }

        std::cout << "Executing build action via ZeroMesh REAPI pipeline...\n";
        zeromesh::executor::ExecutionRequest req{
            .action_id = "test-action-001",
            .command_args = args,
            .env_vars = {},
            .input_root_digest = zeromesh::cas::Digest{"", 0},
            .expected_output_files = {},
            .expected_output_directories = {},
            .working_directory = "."
        };

        auto res = exec_svc->ExecuteAction(req);
        std::cout << "Execution completed. Exit code: " << res.exit_code 
                  << " (Worker: " << res.worker_id << ")\n";
        return res.exit_code;
    }

    PrintUsage(argv[0]);
    return 1;
}
