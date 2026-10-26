#include "cli/team_cmd.hpp"

#include <chrono>
#include <iostream>

namespace zeromesh::cli {

namespace {

void PrintTeamUsage(std::ostream& os) {
    os << "Usage: zm team <subcommand>\n\n"
       << "Subcommands:\n"
       << "  create              Create a team and print the join phrase\n"
       << "  join '<phrase>'     Join a team (quote the phrase; it may contain * ~ $ =)\n"
       << "  leave               Leave the current team\n"
       << "  info                Show team membership\n";
}

} // namespace

int TeamCommand(Client& client, const std::vector<std::string>& args) {
    if (args.empty()) {
        PrintTeamUsage(std::cerr);
        return 1;
    }
    const std::string& sub = args[0];
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));

    if (sub == "create") {
        if (args.size() != 1) {
            std::cerr << "zm team create: unexpected argument\n";
            return 1;
        }
        v1::TeamSecretReply reply;
        grpc::Status s = client.stub().TeamCreate(&ctx, v1::Empty{}, &reply);
        if (!s.ok()) return ReportRpcError("team create", s);
        // Single-quoted: the check symbol can be * ~ $ =.
        std::cout << "'" << reply.phrase() << "'\n";
        return 0;
    }
    if (sub == "join") {
        if (args.size() != 2) {
            std::cerr << "zm team join: needs a phrase (quote it)\n";
            PrintTeamUsage(std::cerr);
            return 1;
        }
        v1::TeamJoinRequest req;
        req.set_phrase(args[1]);
        v1::Empty reply;
        grpc::Status s = client.stub().TeamJoin(&ctx, req, &reply);
        if (!s.ok()) return ReportRpcError("team join", s);
        return 0;
    }
    if (sub == "leave") {
        if (args.size() != 1) {
            std::cerr << "zm team leave: unexpected argument\n";
            return 1;
        }
        v1::Empty reply;
        grpc::Status s = client.stub().TeamLeave(&ctx, v1::Empty{}, &reply);
        if (!s.ok()) return ReportRpcError("team leave", s);
        return 0;
    }
    if (sub == "info") {
        if (args.size() != 1) {
            std::cerr << "zm team info: unexpected argument\n";
            return 1;
        }
        v1::TeamInfoReply reply;
        grpc::Status s = client.stub().TeamInfo(&ctx, v1::Empty{}, &reply);
        if (!s.ok()) return ReportRpcError("team info", s);
        if (!reply.in_team()) {
            std::cout << "not in a team\n";
            return 0;
        }
        std::cout << "team_id:      " << reply.team_id() << "\n"
                  << "peers_pinned: " << reply.peers_pinned() << "\n";
        return 0;
    }

    std::cerr << "zm team: unknown subcommand '" << sub << "'\n\n";
    PrintTeamUsage(std::cerr);
    return 1;
}

} // namespace zeromesh::cli
