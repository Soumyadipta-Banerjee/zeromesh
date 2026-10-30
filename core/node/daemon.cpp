#include "node/daemon.hpp"

#include "plugins/env-host/env_host.hpp"
#include "plugins/env-mirror/env_mirror.hpp"
#include "exec/sandbox_runner.hpp"
#include "net/discovery.hpp"
#include "net/identity.hpp"
#include "net/peer_table.hpp"
#include "net/pin_store.hpp"
#include "net/team.hpp"
#include "net/telemetry.hpp"
#include "net/tls.hpp"
#include "node/action_cache.hpp"
#include "node/client_service.hpp"
#include "node/comfort_governor.hpp"
#include "node/donor_policy.hpp"
#include "node/job_manager.hpp"
#include "node/local_executor.hpp"
#include "node/node_client.hpp"
#include "node/node_service.hpp"
#include "node/state.hpp"
#include "plugin/client_api_handle.hpp"
#include "plugin/loader.hpp"
#include "sched/hazard.hpp"
#include "sched/placer.hpp"
#include "store/merkle.hpp"
#include "store/store.hpp"

#include <grpcpp/grpcpp.h>

#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zeromesh::node {

namespace {

namespace fs = std::filesystem;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v == nullptr ? std::string() : std::string(v);
}

fs::path HomeDir() {
    std::string home = Env("HOME");
    if (!home.empty()) return home;
    if (const passwd* pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr) return pw->pw_dir;
    return "/tmp";
}

int PortFromEnv(const char* name, int def) {
    std::string v = Env(name);
    if (v.empty()) return def;
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || n < 1 || n > 65535) return def;
    return static_cast<int>(n);
}

// Creates `dir` with mode 0700, or checks an existing one is a real directory owned by us,
// then forces its mode to 0700.
void PrepareRuntimeDir(const fs::path& dir) {
    fs::create_directories(dir);
    struct stat st{};
    if (lstat(dir.c_str(), &st) != 0)
        throw std::runtime_error("cannot stat " + dir.string() + ": " + std::strerror(errno));
    if (!S_ISDIR(st.st_mode)) throw std::runtime_error(dir.string() + " is not a directory");
    if (st.st_uid != getuid()) throw std::runtime_error(dir.string() + " is owned by another user");
    if (chmod(dir.c_str(), 0700) != 0) {
        throw std::runtime_error("cannot chmod " + dir.string() + ": " + std::strerror(errno));
    }
}

std::string Hostname() {
    char host[256] = {};
    if (gethostname(host, sizeof(host) - 1) != 0) return "unknown";
    return host;
}

} // namespace

DaemonConfig DaemonConfig::FromEnv() {
    DaemonConfig c;
    const fs::path home = HomeDir();

    std::string v = Env("ZM_STATE_DIR");
    c.state_dir = v.empty() ? home / ".local/share/zeromesh" : fs::path(v);

    v = Env("ZM_RUNTIME_DIR");
    if (!v.empty()) {
        c.runtime_dir = v;
    } else if (std::string xdg = Env("XDG_RUNTIME_DIR"); !xdg.empty()) {
        c.runtime_dir = fs::path(xdg) / "zeromesh";
    } else {
        c.runtime_dir = "/tmp/zeromesh-" + std::to_string(getuid());
    }

    v = Env("ZM_CONFIG_DIR");
    c.config_dir = v.empty() ? home / ".config/zeromesh" : fs::path(v);

    c.node_port = PortFromEnv("ZM_NODE_PORT", 8981);
    c.discovery_port = PortFromEnv("ZM_DISCOVERY_PORT", 8988);
    return c;
}

bool UnixSocketAlive(const fs::path& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::string s = path.string();
    if (s.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, s.c_str(), s.size() + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    int rc;
    do {
        rc = connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    } while (rc != 0 && errno == EINTR);
    close(fd);
    return rc == 0;
}

Daemon::Daemon(DaemonConfig config) : config_(std::move(config)) {
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
}

Daemon::~Daemon() {
    if (wake_fd_ >= 0) close(wake_fd_);
}

void Daemon::Shutdown() {
    uint64_t one = 1;
    ssize_t n = write(wake_fd_, &one, sizeof(one));
    (void)n;
}

int Daemon::Run() {
    // Block SIGINT/SIGTERM before any thread exists, so every thread inherits the mask and
    // the signals are only ever consumed through the signalfd below.
    sigset_t sigs, old_mask;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, &old_mask);
    struct MaskRestore {
        sigset_t mask;
        ~MaskRestore() { pthread_sigmask(SIG_SETMASK, &mask, nullptr); }
    } restore{old_mask};

    const fs::path sock = config_.socket_path();
    try {
        PrepareRuntimeDir(config_.runtime_dir);
    } catch (const std::exception& e) {
        std::cerr << "zmd: " << e.what() << "\n";
        return 1;
    }
    std::error_code ec;
    if (fs::symlink_status(sock, ec).type() != fs::file_type::not_found) {
        if (UnixSocketAlive(sock)) {
            std::cerr << "zmd is already running\n";
            return 1;
        }
        fs::remove(sock, ec); // stale socket from a daemon that died
    }

    int sfd = signalfd(-1, &sigs, SFD_CLOEXEC);
    if (sfd < 0) {
        std::cerr << "zmd: signalfd: " << std::strerror(errno) << "\n";
        return 1;
    }
    struct FdCloser {
        int fd;
        ~FdCloser() { close(fd); }
    } sfd_closer{sfd};

    std::unique_ptr<grpc::Server> client_server;
    std::unique_ptr<grpc::Server> node_server;
    std::shared_ptr<JobManager> jobs;
    std::shared_ptr<LocalExecutor> local_executor;
    std::shared_ptr<sched::Placer> placer;
    std::unique_ptr<ClientService> client_service;
    std::unique_ptr<NodeService> node_service;
    std::shared_ptr<net::PeerTable> peers;
    std::shared_ptr<sched::HazardModel> hazard;      // departures of the peers we place work on
    std::shared_ptr<sched::HazardModel> self_hazard; // this node's own restarts, reported to requesters
    std::shared_ptr<store::CasStore> cas;
    std::shared_ptr<ActionCache> cache;
    std::shared_ptr<net::Discovery> discovery;
    std::shared_ptr<TeamState> team;
    std::shared_ptr<net::PinStore> pins;
    std::shared_ptr<plugin::Registry> registry;
    std::shared_ptr<plugin::ClientApiHandle> frontend_handle;
    net::Identity identity;
    std::string node_id;

    // Admitted peers: node_id -> NodeClient. Cleared on team change.
    std::mutex clients_mu;
    std::unordered_map<std::string, std::shared_ptr<NodeClient>> clients;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> admit_backoff;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> ping_last;

    std::atomic<bool> admit_stop{false};
    std::thread admit_thread;
    std::thread drain_thread;
    std::thread gc_thread;

    std::mutex telemetry_mu;
    net::TelemetrySampler telemetry_sampler;
    auto telemetry_fn = [&]() {
        std::lock_guard lock(telemetry_mu);
        return telemetry_sampler.Sample();
    };
    DrainController drain_controller;
    std::unique_ptr<ComfortGovernor> comfort; // outlives the runner whose observer points at it

    try {
        auto state = NodeState::LoadOrCreate(config_.state_dir);
        node_id = state.node_id();
        identity = net::Identity::LoadOrCreate(config_.state_dir, node_id);
        pins = std::make_shared<net::PinStore>(config_.state_dir);

        store::CasLimits cas_limits;
        if (const char* cap = std::getenv("ZM_CAS_MAX_BYTES"); cap && *cap) {
            cas_limits.max_blob_bytes = std::strtoull(cap, nullptr, 10);
        }
        cas = std::make_shared<store::CasStore>(config_.state_dir / "cas", cas_limits);
        auto merkle = std::make_shared<store::MerkleBuilder>(cas);
        auto runner = std::make_shared<exec::SandboxRunner>(cas, merkle, config_.state_dir / "scratch");
        registry = std::make_shared<plugin::Registry>();
        registry->RegisterEnv(envhost::Make());
        registry->RegisterEnv(mirror::MakeMirrorEnv());
        registry->LoadSearchPath();
        cache = std::make_shared<ActionCache>(cas);
        peers = std::make_shared<net::PeerTable>();
        hazard = std::make_shared<sched::HazardModel>(config_.state_dir / "hazard.log");
        sched::ObservePeers(*peers, hazard);
        self_hazard = std::make_shared<sched::HazardModel>(config_.state_dir / "hazard-self.log");
        self_hazard->NoteJoin(node_id); // a session still open from a crashed run is closed as a departure here

        team = std::make_shared<TeamState>();
        team->state_dir = config_.state_dir;
        team->pins = pins;
        if (auto existing = net::TeamFile::Load(config_.state_dir)) {
            std::lock_guard lock(team->mu);
            team->key = *existing;
        }
        team->on_changed = [&]() {
            std::lock_guard lock(clients_mu);
            clients.clear();
            admit_backoff.clear();
            ping_last.clear();
        };

        net::DiscoveryConfig disc_cfg;
        disc_cfg.node_id = node_id;
        disc_cfg.hostname = Hostname();
        disc_cfg.node_port = config_.node_port;
        disc_cfg.discovery_port = config_.discovery_port;
        disc_cfg.team = team->Get();
        disc_cfg.telemetry = telemetry_fn;
        discovery = std::make_shared<net::Discovery>(peers, disc_cfg);
        team->discovery = discovery;
        if (!discovery->Start()) throw std::runtime_error("cannot start discovery");

        auto client_factory = [&](const net::Peer& p) -> std::shared_ptr<NodeClient> {
            if (p.ip_address.empty() || p.node_port <= 0) return nullptr;
            std::lock_guard lock(clients_mu);
            if (auto it = clients.find(p.id); it != clients.end()) return it->second;
            if (!pins->FingerprintFor(p.id)) return nullptr;
            auto c =
                std::make_shared<NodeClient>(p.ip_address + ":" + std::to_string(p.node_port), identity, pins, p.id);
            clients[p.id] = c;
            return c;
        };

        NodeClientFactory client_for_node = [&](const std::string& peer_id) -> std::shared_ptr<NodeClient> {
            auto peer = peers->Get(peer_id);
            return peer ? client_factory(*peer) : nullptr;
        };
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        int slots = std::max(1, hw - 1);
        local_executor = std::make_shared<LocalExecutor>(runner, registry, node_id, slots,
                                                         config_.state_dir / "scratch", cas, client_for_node);

        placer =
            std::make_shared<sched::Placer>(local_executor, peers, client_factory, cas, merkle, node_id, telemetry_fn);
        placer->SetHazardModel(hazard);
        jobs = std::make_shared<JobManager>(cas, merkle, cache, placer);

        ClientServiceOptions opts;
        opts.node_id = node_id;
        opts.node_port = config_.node_port;
        opts.upload_limit_bytes = UploadLimitFromEnv();
        client_service = std::make_unique<ClientService>(cas, merkle, jobs, peers, opts, team, local_executor);

        DonorPolicy policy = DonorPolicy::Load(config_.config_dir);
        drain_controller.SetPolicy(policy);
        comfort = std::make_unique<ComfortGovernor>(ComfortConfig::FromPolicy(policy));
        runner->SetCgroupObserver([g = comfort.get()](const std::filesystem::path& dir, bool added) {
            if (added)
                g->AddTaskCgroup(dir);
            else
                g->RemoveTaskCgroup(dir);
        });
        node_service = std::make_unique<NodeService>(
            node_id, pins, [team]() { return team->Get(); }, identity.fingerprint,
            [svc = client_service.get()]() { return svc->SelfInfo(); }, cas, registry, local_executor, telemetry_fn,
            policy, peers);
        node_service->SetComfortLevelProvider([g = comfort.get()] { return g->level(); });
        node_service->SetHazardProvider([m = self_hazard, id = node_id] {
            return m->Observations(id) > 0 ? m->HazardPerHour(id) : -1.0; // -1: no record yet
        });
        comfort->Start();

        grpc::ServerBuilder client_builder;
        client_builder.AddListeningPort("unix:" + sock.string(), grpc::InsecureServerCredentials());
        client_builder.RegisterService(client_service.get());
        mode_t old_umask = umask(0177); // the socket is created 0600, closing the chmod race
        client_server = client_builder.BuildAndStart();
        umask(old_umask);
        if (!client_server) throw std::runtime_error("cannot listen on " + sock.string());
        if (chmod(sock.c_str(), 0600) != 0) {
            throw std::runtime_error("cannot chmod " + sock.string() + ": " + std::strerror(errno));
        }

        // Front-ends (e.g. frontend-reapi) talk back to this daemon over the ClientApi unix socket.
        // Keep the handle alive for the daemon lifetime — ReapiServer holds a reference into it.
        frontend_handle = std::make_shared<plugin::ClientApiHandle>(
            sock.string(), grpc::CreateChannel("unix:" + sock.string(), grpc::InsecureChannelCredentials()));
        plugin::PluginConfig frontend_cfg;
        if (const char* en = std::getenv("ZM_ENABLE_REAPI")) {
            frontend_cfg.values["enabled"] = en;
        }
        registry->StartFrontends(*frontend_handle, frontend_cfg);

        grpc::ServerBuilder node_builder;
        node::ApplyNodeApiMessageLimits(node_builder);
        net::AllowKeepalivePings(node_builder);
        node_builder.AddListeningPort("0.0.0.0:" + std::to_string(config_.node_port), net::ServerCreds(identity));
        node_builder.RegisterService(node_service.get());
        node_server = node_builder.BuildAndStart();
        if (!node_server) {
            throw std::runtime_error("cannot listen on NodeApi 0.0.0.0:" + std::to_string(config_.node_port));
        }

        admit_thread = std::thread([&]() {
            while (!admit_stop.load()) {
                self_hazard->NoteAlive(node_id); // dates a later crash; logged at most once a minute
                auto team_key = team->Get();
                if (team_key) {
                    peers->PruneStale();
                    // Admit anyone we can hear (including draining). Hiding drainers
                    // from ListActive made typing peers invisible and never pinable.
                    for (const auto& p : peers->ListPresent()) {
                        if (p.id == node_id) continue;
                        if (pins->FingerprintFor(p.id)) {
                            (void)client_factory(p);
                            std::shared_ptr<NodeClient> client;
                            {
                                std::lock_guard lock(clients_mu);
                                if (auto it = clients.find(p.id); it != clients.end()) client = it->second;
                            }
                            if (client) {
                                const auto now = std::chrono::steady_clock::now();
                                bool due = true;
                                {
                                    std::lock_guard lock(clients_mu);
                                    if (auto it = ping_last.find(p.id);
                                        it != ping_last.end() && now - it->second < std::chrono::seconds(5)) {
                                        due = false;
                                    }
                                }
                                if (due) {
                                    std::string err;
                                    if (auto rtt = client->PingRtt(&err)) {
                                        peers->RecordMeasuredLatency(p.id, *rtt);
                                        std::lock_guard lock(clients_mu);
                                        ping_last[p.id] = now;
                                    }
                                }
                            }
                            continue;
                        }
                        auto now = std::chrono::steady_clock::now();
                        {
                            std::lock_guard lock(clients_mu);
                            if (auto it = admit_backoff.find(p.id); it != admit_backoff.end() && now < it->second) {
                                continue;
                            }
                        }
                        if (p.ip_address.empty() || p.node_port <= 0) continue;
                        auto c = std::make_shared<NodeClient>(p.ip_address + ":" + std::to_string(p.node_port),
                                                              identity, pins, p.id);
                        std::string err;
                        if (c->Admit(*team_key, &err)) {
                            std::lock_guard lock(clients_mu);
                            clients[p.id] = std::move(c);
                            admit_backoff.erase(p.id);
                        } else {
                            std::lock_guard lock(clients_mu);
                            admit_backoff[p.id] = now + std::chrono::seconds(30);
                        }
                    }
                }
                for (int i = 0; i < 20 && !admit_stop.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        // Sticky drain (M4): sample telemetry, keep discovery announcements draining until
        // the hold period clears, notify requesters, and cancel preemptible donor tasks.
        drain_thread = std::thread([&]() {
            while (!admit_stop.load()) {
                const net::Telemetry t = telemetry_fn();
                const auto upd = drain_controller.Tick(t);
                if (upd.entered) {
                    std::cerr << "zmd: draining: " << upd.reason << "\n";
                    if (discovery) discovery->BroadcastNodeDraining(upd.reason);
                    if (node_service) {
                        node_service->SetDraining(true, upd.reason);
                        for (const auto& req_id : node_service->RequestersWithWork()) {
                            std::shared_ptr<NodeClient> c;
                            {
                                std::lock_guard lock(clients_mu);
                                if (auto it = clients.find(req_id); it != clients.end()) c = it->second;
                            }
                            if (!c && peers) {
                                if (auto peer = peers->Get(req_id); peer && !peer->ip_address.empty() &&
                                                                    peer->node_port > 0 &&
                                                                    pins->FingerprintFor(peer->id)) {
                                    c = std::make_shared<NodeClient>(peer->ip_address + ":" +
                                                                         std::to_string(peer->node_port),
                                                                     identity, pins, peer->id);
                                    std::lock_guard lock(clients_mu);
                                    clients[peer->id] = c;
                                }
                            }
                            if (c) {
                                std::string err;
                                c->Drain(upd.reason, &err);
                            }
                        }
                        node_service->CancelPreemptible();
                    }
                } else if (upd.left) {
                    std::cerr << "zmd: resume accepting work\n";
                    if (discovery) {
                        discovery->SetLocalState(net::PeerState::Active);
                        discovery->AnnounceNow();
                    }
                    if (node_service) node_service->SetDraining(false);
                } else if (upd.draining && discovery && discovery->local_state() != net::PeerState::Draining) {
                    discovery->SetLocalState(net::PeerState::Draining);
                }
                for (int i = 0; i < 10 && !admit_stop.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        // Reclaim CAS toward 90% of the cap. Protected digests are inputs of live jobs.
        gc_thread = std::thread([&]() {
            while (!admit_stop.load()) {
                if (cas) {
                    const uint64_t target = cas->MaxBlobBytes() * 9 / 10;
                    std::unordered_set<std::string> protect;
                    if (node_service) {
                        for (const auto& h : node_service->ProtectedBlobHashes()) protect.insert(h);
                    }
                    if (jobs) {
                        for (const auto& h : jobs->ProtectedBlobHashes()) protect.insert(h);
                    }
                    cas->CollectGarbage(target, protect);
                }
                if (cache) cache->PruneStale();
                for (int i = 0; i < 300 && !admit_stop.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
            }
        });

        std::cerr << "zmd: node " << node_id << " listening on " << sock.string()
                  << " and NodeApi :" << config_.node_port << "\n";
    } catch (const std::exception& e) {
        std::cerr << "zmd: " << e.what() << "\n";
        admit_stop = true;
        if (admit_thread.joinable()) admit_thread.join();
        if (drain_thread.joinable()) drain_thread.join();
        if (gc_thread.joinable()) gc_thread.join();
        if (registry) registry->StopFrontends();
        frontend_handle.reset();
        if (discovery) discovery->Stop();
        if (node_server) node_server->Shutdown();
        if (client_server) client_server->Shutdown();
        fs::remove(sock, ec);
        return 1;
    }

    pollfd fds[2] = {{sfd, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
    for (;;) {
        int rc = poll(fds, 2, -1);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) {
            std::cerr << "zmd: poll: " << std::strerror(errno) << "\n";
            break;
        }
        if (fds[0].revents & POLLIN) {
            signalfd_siginfo si{};
            if (read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
                std::cerr << "zmd: " << strsignal(static_cast<int>(si.ssi_signo)) << ", shutting down\n";
            }
            break;
        }
        if (fds[1].revents & POLLIN) break;
    }

    // Stop admission and drain monitor, then discovery; Drain peers with tasks in flight.
    admit_stop = true;
    if (comfort) comfort->Stop();
    if (admit_thread.joinable()) admit_thread.join();
    if (drain_thread.joinable()) drain_thread.join();
    if (gc_thread.joinable()) gc_thread.join();
    if (self_hazard) self_hazard->NoteDrain(node_id); // clean stop: censored, not a failure
    if (discovery) {
        discovery->BroadcastNodeDraining("shutting down");
        discovery->Stop();
    }
    if (placer) {
        for (const auto& peer_id : placer->NodesWithInFlight()) {
            std::shared_ptr<NodeClient> c;
            {
                std::lock_guard lock(clients_mu);
                if (auto it = clients.find(peer_id); it != clients.end()) c = it->second;
            }
            if (c) {
                std::string err;
                c->Drain("shutting down", &err);
            }
        }
    }

    if (registry) registry->StopFrontends();
    frontend_handle.reset();

    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
    if (node_server) node_server->Shutdown(deadline);
    if (client_server) client_server->Shutdown(deadline);
    if (node_server) node_server->Wait();
    if (client_server) client_server->Wait();
    node_server.reset();
    client_server.reset();
    node_service.reset();
    client_service.reset();
    jobs.reset();   // cancels unfinished jobs
    placer.reset(); // cancels remote work and joins workers
    {
        std::lock_guard lock(clients_mu);
        clients.clear();
    }
    local_executor.reset(); // kills running sandboxes and joins its workers
    discovery.reset();
    team.reset();
    pins.reset();
    fs::remove(sock, ec);
    return 0;
}

} // namespace zeromesh::node
