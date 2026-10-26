#include "cli/status_cmd.hpp"

#include "cli/tui.hpp"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <iostream>

namespace zeromesh::cli {

namespace {

std::atomic<bool> g_stop{false};

void OnInterrupt(int) { g_stop.store(true); }

grpc::Status ListPeers(Client& client, v1::PeerList* out) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    return client.stub().ListPeers(&ctx, v1::Empty{}, out);
}

} // namespace

int StatusCommand(Client& client) {
    v1::PeerList peers;
    grpc::Status s = ListPeers(client, &peers);
    if (!s.ok()) return ReportRpcError("cannot list peers", s);
    RenderStatus(peers, std::cout, isatty(STDOUT_FILENO) != 0);
    return 0;
}

int MonitorCommand(Client& client) {
    struct sigaction sa {};
    sa.sa_handler = OnInterrupt;  // no SA_RESTART: poll() returns EINTR at once
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int rc = RunMonitor(
        [&client](std::string* error, bool* unavailable) -> std::optional<v1::PeerList> {
            v1::PeerList peers;
            grpc::Status s = ListPeers(client, &peers);
            if (s.ok()) return peers;
            *unavailable = s.error_code() == grpc::StatusCode::UNAVAILABLE;
            *error = s.error_message();
            return std::nullopt;
        },
        g_stop);
    if (rc == kExitNotRunning) std::cerr << kNotRunning << "\n";
    return rc;
}

} // namespace zeromesh::cli
