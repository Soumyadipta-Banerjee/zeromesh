#pragma once

// An in-process node for NodeApi tests: its own state dir, identity, pins, CAS,
// env-host registry, LocalExecutor (ZM_NODE_ID = node id) and a NodeService on
// 127.0.0.1 with mTLS. Telemetry is fixed (on AC, 8 cores, arch "testarch").

#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "exec/sandbox_runner.hpp"
#include "net/identity.hpp"
#include "net/peer_table.hpp"
#include "net/pin_store.hpp"
#include "net/team.hpp"
#include "net/telemetry.hpp"
#include "net/tls.hpp"
#include "node/donor_policy.hpp"
#include "node/local_executor.hpp"
#include "node/node_client.hpp"
#include "node/node_service.hpp"
#include "plugin/loader.hpp"
#include "plugins/env-host/env_host.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <memory>
#include <optional>
#include <string>

namespace zeromesh::testing {

inline constexpr const char* kTestArch = "testarch";

inline net::Telemetry TestTelemetry() {
    net::Telemetry t;
    t.cores_total = 8;
    t.cores_free = 8;
    t.mem_total = 8ull << 30;
    t.mem_available = 8ull << 30;
    t.cpu_load_pct = 5.0;
    t.battery_pct = -1;
    t.on_ac = true;
    t.arch = kTestArch;
    return t;
}

struct TestNodeFull {
    TempDir dir;
    std::string node_id;
    net::Identity id;
    std::shared_ptr<net::PinStore> pins;
    net::TeamKey team;
    std::shared_ptr<store::CasStore> cas;
    std::shared_ptr<store::MerkleBuilder> merkle;
    std::shared_ptr<plugin::Registry> registry;
    std::shared_ptr<exec::SandboxRunner> runner;
    std::shared_ptr<node::LocalExecutor> executor;
    std::shared_ptr<net::PeerTable> peers = std::make_shared<net::PeerTable>();
    net::Telemetry telemetry = TestTelemetry();
    node::DonorPolicy policy;
    std::unique_ptr<node::NodeService> service;
    std::unique_ptr<grpc::Server> server;
    int port = 0;

    TestNodeFull(std::string name, net::TeamKey key)
        : node_id(std::move(name)),
          id(net::Identity::LoadOrCreate(dir / "state", node_id)),
          pins(std::make_shared<net::PinStore>(dir / "state")),
          team(key),
          cas(std::make_shared<store::CasStore>(dir / "cas")),
          merkle(std::make_shared<store::MerkleBuilder>(cas)),
          registry(std::make_shared<plugin::Registry>()) {
        registry->RegisterEnv(envhost::Make());
        runner = std::make_shared<exec::SandboxRunner>(cas, merkle, dir / "scratch");
        executor = std::make_shared<node::LocalExecutor>(runner, registry, node_id, 2);
    }

    ~TestNodeFull() { Stop(); }

    TestNodeFull(const TestNodeFull&) = delete;
    TestNodeFull& operator=(const TestNodeFull&) = delete;

    void Start(int want_port = 0) {
        service = std::make_unique<node::NodeService>(
            node_id, pins, [k = team] { return std::optional<net::TeamKey>(k); }, id.fingerprint,
            [n = node_id] {
                v1::PeerInfo info;
                info.set_node_id(n);
                return info;
            },
            cas, registry, executor, [this] { return telemetry; }, policy, peers);
        grpc::ServerBuilder b;
        node::ApplyNodeApiMessageLimits(b);
        net::AllowKeepalivePings(b);
        b.AddListeningPort("127.0.0.1:" + std::to_string(want_port), net::ServerCreds(id), &port);
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

    std::shared_ptr<node::NodeClient> ClientTo(const TestNodeFull& peer) const {
        return std::make_shared<node::NodeClient>(peer.address(), id, pins, peer.node_id);
    }

    // Runs admission with `peer`; afterwards both have pinned each other.
    void Admit(const TestNodeFull& peer) const {
        node::NodeClient c(peer.address(), id, pins);
        std::string err;
        if (!c.Admit(team, &err)) throw std::runtime_error("admission failed: " + err);
    }

    // `peer` as discovery would have announced it.
    net::Peer AsPeer() const {
        net::Peer p;
        p.id = node_id;
        p.hostname = node_id;
        p.arch = telemetry.arch;
        p.ip_address = "127.0.0.1";
        p.node_port = port;
        p.cores = telemetry.cores_total;
        p.cores_free = telemetry.cores_free;
        p.memory_bytes = telemetry.mem_total;
        p.mem_available = telemetry.mem_available;
        p.cpu_load_pct = telemetry.cpu_load_pct;
        p.battery_pct = telemetry.battery_pct;
        p.is_charging = telemetry.on_ac;
        return p;
    }
};

} // namespace zeromesh::testing
