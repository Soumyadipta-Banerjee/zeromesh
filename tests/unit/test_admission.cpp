#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "net/identity.hpp"
#include "net/pin_store.hpp"
#include "net/team.hpp"
#include "net/tls.hpp"
#include "node/node_client.hpp"
#include "node/node_service.hpp"

#include <grpcpp/client_context.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <sys/stat.h>

#include <memory>
#include <optional>
#include <string>

using zeromesh::net::Identity;
using zeromesh::net::PinStore;
using zeromesh::net::TeamKey;
using zeromesh::node::NodeClient;
using zeromesh::node::NodeService;
using zeromesh::testing::TempDir;

namespace {

// One in-process node: identity + pins in its own temp state dir, NodeApi on 127.0.0.1.
struct TestNode {
    TempDir dir;
    std::string node_id;
    Identity id;
    std::shared_ptr<PinStore> pins;
    TeamKey team;
    std::unique_ptr<NodeService> service;
    std::unique_ptr<grpc::Server> server;
    int port = 0;

    TestNode(std::string name, TeamKey key)
        : node_id(std::move(name)),
          id(Identity::LoadOrCreate(dir.path(), node_id)),
          pins(std::make_shared<PinStore>(dir.path())),
          team(key) {}

    ~TestNode() { Stop(); }

    void Start(int want_port = 0) {
        service = std::make_unique<NodeService>(
            node_id, pins, [k = team] { return std::optional<TeamKey>(k); }, id.fingerprint, [n = node_id] {
                zeromesh::v1::PeerInfo info;
                info.set_node_id(n);
                return info;
            });
        grpc::ServerBuilder b;
        b.AddListeningPort("127.0.0.1:" + std::to_string(want_port), zeromesh::net::ServerCreds(id), &port);
        b.RegisterService(service.get());
        server = b.BuildAndStart();
        ASSERT_TRUE(server != nullptr);
        ASSERT_TRUE(port > 0);
    }

    void Stop() {
        if (server) server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
        server.reset();
        service.reset();
    }

    std::string address() const { return "127.0.0.1:" + std::to_string(port); }

    NodeClient ClientTo(const TestNode& peer) const { return NodeClient(peer.address(), id, pins); }
};

// Calls Ping on `peer` through a channel that accepts any server certificate, so the
// result shows only the server-side pin gate.
grpc::StatusCode RawPing(const TestNode& from, const TestNode& peer) {
    auto channel = zeromesh::net::PeerChannel(peer.address(),
                                              zeromesh::net::ClientCreds(from.id, [](std::string_view) { return true; }));
    auto stub = zeromesh::v1::NodeApi::NewStub(channel);
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    zeromesh::v1::PeerInfo info;
    return stub->Ping(&ctx, zeromesh::v1::Empty{}, &info).error_code();
}

} // namespace

TEST(AdmissionSameTeamSucceeds) {
    TeamKey key = zeromesh::net::GenerateTeamKey();
    TestNode a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();

    NodeClient client = a.ClientTo(b);
    std::string err;
    ASSERT_TRUE(client.Admit(key, &err));
    ASSERT_EQ(a.pins->FingerprintFor("node-b"), std::optional<std::string>(b.id.fingerprint));
    ASSERT_EQ(b.pins->FingerprintFor("node-a"), std::optional<std::string>(a.id.fingerprint));
    // Write-through, mode 0600, and a reload sees the pin.
    struct stat st{};
    ASSERT_EQ(::stat((a.dir / "peers").c_str(), &st), 0);
    ASSERT_EQ(st.st_mode & 0777, 0600u);
    ASSERT_TRUE(PinStore(a.dir.path()).IsPinned(b.id.fingerprint));

    auto info = client.Ping(&err);
    ASSERT_TRUE(info.has_value());
    ASSERT_EQ(info->node_id(), std::string("node-b"));
}

TEST(AdmissionWrongTeamFails) {
    TestNode a("node-a", zeromesh::net::GenerateTeamKey());
    TestNode b("node-b", zeromesh::net::GenerateTeamKey());
    a.Start();
    b.Start();

    NodeClient client = a.ClientTo(b);
    std::string err;
    ASSERT_FALSE(client.Admit(a.team, &err));
    ASSERT_FALSE(a.pins->IsPinned(b.id.fingerprint));
    ASSERT_FALSE(b.pins->IsPinned(a.id.fingerprint));
    ASSERT_FALSE(client.Ping(&err).has_value());
    ASSERT_EQ(RawPing(a, b), grpc::StatusCode::PERMISSION_DENIED);
}

TEST(UnpinnedPeerDenied) {
    TeamKey key = zeromesh::net::GenerateTeamKey();
    TestNode a("node-a", key), b("node-b", key);
    b.Start();
    ASSERT_EQ(RawPing(a, b), grpc::StatusCode::PERMISSION_DENIED);
    // A pin for the client alone is not enough: the server must have pinned it.
    a.pins->Pin("node-b", b.id.fingerprint);
    NodeClient client(b.address(), a.id, a.pins, std::string("node-b"));
    std::string err;
    ASSERT_FALSE(client.Ping(&err).has_value());
}

TEST(PinnedChannelRejectsImpostor) {
    TeamKey key = zeromesh::net::GenerateTeamKey();
    TestNode a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    NodeClient client = a.ClientTo(b);
    std::string err;
    ASSERT_TRUE(client.Admit(key, &err));
    ASSERT_TRUE(client.Ping(&err).has_value());

    // Same node id, same port, a new key pair; it even trusts A, so only A's pin can refuse it.
    const int port = b.port;
    b.Stop();
    TestNode impostor("node-b", key);
    impostor.pins->Pin("node-a", a.id.fingerprint);
    impostor.Start(port);
    ASSERT_NE(impostor.id.fingerprint, b.id.fingerprint);

    ASSERT_FALSE(client.Ping(&err).has_value());
    ASSERT_EQ(a.pins->FingerprintFor("node-b"), std::optional<std::string>(b.id.fingerprint));
}

TEST(IdentityPersists) {
    TempDir dir;
    Identity first = Identity::LoadOrCreate(dir.path(), "node-a");
    Identity again = Identity::LoadOrCreate(dir.path(), "node-a");
    ASSERT_EQ(first.fingerprint, again.fingerprint);
    ASSERT_TRUE(zeromesh::net::IsFingerprint(first.fingerprint));
    ASSERT_EQ(zeromesh::net::FingerprintOfPem(again.cert_pem), first.fingerprint);
    struct stat st{};
    ASSERT_EQ(::stat((dir / "node.key").c_str(), &st), 0);
    ASSERT_EQ(st.st_mode & 0777, 0600u);
    TempDir other;
    ASSERT_NE(Identity::LoadOrCreate(other.path(), "node-a").fingerprint, first.fingerprint);
}
