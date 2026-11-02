#pragma once

// An in-process node for NodeApi tests: its own state dir, identity, pins, CAS, env-host
// registry, BatchRunner, LocalExecutor (ZM_NODE_ID = node id) and a NodeService on 127.0.0.1 with
// mTLS. Telemetry is fixed (on AC, 8 cores, arch "testarch").

#include "tests/unit/test_framework.hpp"
#include "tests/unit/test_util.hpp"

#include "exec/batch_runner.hpp"
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
#include "sched/placer.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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
    std::shared_ptr<exec::BatchRunner> runner;
    std::shared_ptr<node::LocalExecutor> executor;
    std::shared_ptr<net::PeerTable> peers = std::make_shared<net::PeerTable>();
    net::Telemetry telemetry = TestTelemetry();
    node::DonorPolicy policy;
    std::unique_ptr<node::NodeService> service;
    std::unique_ptr<grpc::Server> server;
    int port = 0;

    TestNodeFull(std::string name, net::TeamKey key)
        : node_id(std::move(name)), id(net::Identity::LoadOrCreate(dir / "state", node_id)),
          pins(std::make_shared<net::PinStore>(dir / "state")), team(key),
          cas(std::make_shared<store::CasStore>(dir / "cas")), merkle(std::make_shared<store::MerkleBuilder>(cas)),
          registry(std::make_shared<plugin::Registry>()) {
        registry->RegisterEnv(envhost::Make());
        runner = std::make_shared<exec::BatchRunner>(cas, merkle, dir / "scratch", exec::cgroup::SetUpWork(0));
        executor = std::make_shared<node::LocalExecutor>(runner, registry, node_id, 2, dir / "state", cas,
                                                         node::NodeClientFactory{});
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
        p.gpus = telemetry.gpus;
        return p;
    }
};

// A batch of shell items: {id, script}.
inline v1::BatchSpec ShBatch(const std::vector<std::pair<std::string, std::string>>& items, v1::Digest root = {},
                             v1::Where where = v1::REMOTE_ONLY) {
    v1::BatchSpec b;
    b.set_name("test/batch");
    *b.mutable_input_root() = std::move(root);
    b.mutable_resources()->set_cores(1);
    b.set_timeout_s(3600);
    b.set_preemptible(true);
    b.set_where(where);
    for (const auto& [id, script] : items) {
        auto* it = b.add_items();
        it->set_id(id);
        it->add_argv("sh");
        it->add_argv("-c");
        it->add_argv(script);
    }
    return b;
}

// Everything one Execute reported, and a way to wait for its end.
struct AttemptLog {
    std::mutex mu;
    std::condition_variable cv;
    bool done{false};
    node::BatchDone result;
    std::vector<std::pair<v1::ItemOutcome, std::string>> items; // outcome, node
    std::vector<std::string> placed;
    std::vector<std::string> started;
    std::string logs;
    std::vector<std::tuple<std::string, v1::RejectKind, std::string>> rejects;

    node::BatchCallbacks Callbacks() {
        node::BatchCallbacks cb;
        cb.on_placed = [this](const std::string& n) { Add([&] { placed.push_back(n); }); };
        cb.on_started = [this](const std::string& id) { Add([&] { started.push_back(id); }); };
        cb.on_log = [this](const v1::ItemLog& l) { Add([&] { logs += l.chunk(); }); };
        cb.on_item = [this](const v1::ItemOutcome& o, const std::string& n) { Add([&] { items.emplace_back(o, n); }); };
        cb.on_reject = [this](const std::string& n, v1::RejectKind k, const std::string& why) {
            Add([&] { rejects.emplace_back(n, k, why); });
        };
        cb.on_done = [this](node::BatchDone d) {
            Add([&] {
                result = std::move(d);
                done = true;
            });
        };
        return cb;
    }
    // True once the attempt ended within `limit`.
    bool Wait(std::chrono::milliseconds limit = std::chrono::seconds(30)) {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, limit, [&] { return done; });
    }
    bool WaitStarted(std::chrono::milliseconds limit = std::chrono::seconds(10)) {
        std::unique_lock lock(mu);
        return cv.wait_for(lock, limit, [&] { return !started.empty() || done; }) && !started.empty();
    }

private:
    template <class F>
    void Add(F f) {
        {
            std::lock_guard lock(mu);
            f();
        }
        cv.notify_all();
    }
};

// A placer over `self` (its peer table, executor and store), and the attempts handed to it.
struct PlacerRequester {
    // Declared first so they outlive the placer: its workers report into them until it is destroyed.
    std::vector<std::unique_ptr<AttemptLog>> logs;
    std::shared_ptr<sched::Placer> placer;

    explicit PlacerRequester(TestNodeFull& self) {
        auto factory = [&self](const net::Peer& p) {
            return std::make_shared<node::NodeClient>(p.ip_address + ":" + std::to_string(p.node_port), self.id,
                                                      self.pins, p.id);
        };
        placer = std::make_shared<sched::Placer>(self.executor, self.peers, factory, self.cas, self.merkle,
                                                 self.node_id, [&self] { return self.telemetry; });
    }
    AttemptLog& Execute(const v1::BatchSpec& spec, std::set<std::string> exclude = {}) {
        logs.push_back(std::make_unique<AttemptLog>());
        placer->Execute("run/b" + std::to_string(logs.size()), spec, std::move(exclude), logs.back()->Callbacks());
        return *logs.back();
    }
    // Waits for every attempt; throws if one did not end or failed as a whole.
    void WaitAll(std::chrono::milliseconds limit = std::chrono::seconds(60)) {
        for (auto& l : logs) {
            if (!l->Wait(limit)) throw std::runtime_error("a batch did not finish");
            if (l->result.failure != v1::FAILURE_NONE) throw std::runtime_error("a batch failed: " + l->result.message);
        }
    }
};

} // namespace zeromesh::testing
