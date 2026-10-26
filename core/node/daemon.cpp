#include "node/daemon.hpp"

#include "plugins/env-host/env_host.hpp"
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
#include "node/donor_policy.hpp"
#include "node/job_manager.hpp"
#include "node/local_executor.hpp"
#include "node/node_client.hpp"
#include "node/node_service.hpp"
#include "node/state.hpp"
#include "plugin/loader.hpp"
#include "plugin/client_api_handle.hpp"
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
    struct stat st {};
    if (lstat(dir.c_str(), &st) != 0) throw std::runtime_error("cannot stat " + dir.string() + ": " + std::strerror(errno));
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
    sockaddr_un addr {};
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
        fs::remove(sock, ec);  // stale socket from a daemon that died
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
    std::shared_ptr<plugin::ClientApiHandle> frontend_handle;
    std::vector<plugin::IFrontend*> active_frontends;
    std::shared_ptr<JobManager> jobs;
    std::shared_ptr<LocalExecutor> local_executor;
    std::shared_ptr<sched::Placer> placer;
    std::unique_ptr<ClientService> client_service;
    std::unique_ptr<NodeService> node_service;
    std::shared_ptr<net::Discovery> discovery;
    std::shared_ptr<TeamState> team;
    std::shared_ptr<net::PinStore> pins;
    net::Identity identity;
    std::string node_id;

    // Admitted peers: node_id -> NodeClient. Cleared on team change.
    std::mutex clients_mu;
    std::unordered_map<std::string, std::shared_ptr<NodeClient>> clients;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> admit_backoff;

    std::atomic<bool> admit_stop{false};
    std::thread admit_thread;

    std::mutex telemetry_mu;
    net::TelemetrySampler telemetry_sampler;
    auto telemetry_fn = [&]() {
        std::lock_guard lock(telemetry_mu);
        return telemetry_sampler.Sample();
    };

    try {
        auto state = NodeState::LoadOrCreate(config_.state_dir);
        node_id = state.node_id();
        identity = net::Identity::LoadOrCreate(config_.state_dir, node_id);
        pins = std::make_shared<net::PinStore>(config_.state_dir);

        auto cas = std::make_shared<store::CasStore>(config_.state_dir / "cas");
        auto merkle = std::make_shared<store::MerkleBuilder>(cas);
        auto runner = std::make_shared<exec::SandboxRunner>(cas, merkle, config_.state_dir / "scratch");
        auto registry = std::make_shared<plugin::Registry>();
        registry->RegisterEnv(envhost::Make());
        registry->LoadSearchPath();
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        int slots = std::max(1, hw - 1);
        local_executor = std::make_shared<LocalExecutor>(runner, registry, node_id, slots);
        auto cache = std::make_shared<ActionCache>(cas);
        auto peers = std::make_shared<net::PeerTable>();

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
            auto c = std::make_shared<NodeClient>(p.ip_address + ":" + std::to_string(p.node_port), identity, pins,
                                                    p.id);
            clients[p.id] = c;
            return c;
        };

        placer = std::make_shared<sched::Placer>(local_executor, peers, client_factory, cas, merkle, node_id,
                                                 telemetry_fn);
        jobs = std::make_shared<JobManager>(cas, merkle, cache, placer);

        ClientServiceOptions opts;
        opts.node_id = node_id;
        opts.node_port = config_.node_port;
        opts.upload_limit_bytes = UploadLimitFromEnv();
        client_service = std::make_unique<ClientService>(cas, merkle, jobs, peers, opts, team);

        DonorPolicy policy = DonorPolicy::Load(config_.config_dir);
        node_service = std::make_unique<NodeService>(
            node_id, pins, [team]() { return team->Get(); }, identity.fingerprint,
            [svc = client_service.get()]() { return svc->SelfInfo(); }, cas, registry, local_executor, telemetry_fn,
            policy, peers);

        grpc::ServerBuilder client_builder;
        client_builder.AddListeningPort("unix:" + sock.string(), grpc::InsecureServerCredentials());
        client_builder.RegisterService(client_service.get());
        mode_t old_umask = umask(0177);  // the socket is created 0600, closing the chmod race
        client_server = client_builder.BuildAndStart();
        umask(old_umask);
        if (!client_server) throw std::runtime_error("cannot listen on " + sock.string());
        if (chmod(sock.c_str(), 0600) != 0) {
            throw std::runtime_error("cannot chmod " + sock.string() + ": " + std::strerror(errno));
        }

        grpc::ServerBuilder node_builder;
        net::AllowKeepalivePings(node_builder);
        node_builder.AddListeningPort("0.0.0.0:" + std::to_string(config_.node_port), net::ServerCreds(identity));
        node_builder.RegisterService(node_service.get());
        node_server = node_builder.BuildAndStart();
        if (!node_server) {
            throw std::runtime_error("cannot listen on NodeApi 0.0.0.0:" + std::to_string(config_.node_port));
        }

        // Start any front-end plugins (e.g. frontend-reapi) against the daemon's own ClientApi.
        frontend_handle = std::make_shared<plugin::ClientApiHandle>(
            sock.string(),
            grpc::CreateChannel("unix:" + sock.string(), grpc::InsecureChannelCredentials()));
        for (plugin::IFrontend* fe : registry->Frontends()) {
            plugin::PluginConfig cfg;
            std::string err;
            try {
                if (!fe->Start(*frontend_handle, cfg)) {
                    std::cerr << "zmd: front-end '" << fe->Name() << "' failed to start\n";
                } else {
                    active_frontends.push_back(fe);
                    std::cerr << "zmd: front-end '" << fe->Name() << "' started\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "zmd: front-end '" << fe->Name() << "' threw: " << e.what() << "\n";
            }
        }

        admit_thread = std::thread([&]() {
            while (!admit_stop.load()) {
                auto team_key = team->Get();
                if (team_key) {
                    peers->PruneStale();
                    for (const auto& p : peers->ListActive()) {
                        if (p.id == node_id) continue;
                        if (pins->FingerprintFor(p.id)) {
                            // Ensure a cached client exists for already-pinned peers.
                            (void)client_factory(p);
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

        std::cerr << "zmd: node " << node_id << " listening on " << sock.string() << " and NodeApi :"
                  << config_.node_port << "\n";
    } catch (const std::exception& e) {
        std::cerr << "zmd: " << e.what() << "\n";
        admit_stop = true;
        if (admit_thread.joinable()) admit_thread.join();
        for (plugin::IFrontend* fe : active_frontends) { try { fe->Stop(); } catch (...) {} }
        active_frontends.clear();
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
            signalfd_siginfo si {};
            if (read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
                std::cerr << "zmd: " << strsignal(static_cast<int>(si.ssi_signo)) << ", shutting down\n";
            }
            break;
        }
        if (fds[1].revents & POLLIN) break;
    }

    // Stop admission, then discovery; Drain peers with tasks in flight; shut down both servers.
    admit_stop = true;
    if (admit_thread.joinable()) admit_thread.join();
    for (plugin::IFrontend* fe : active_frontends) {
        try { fe->Stop(); } catch (...) {}
    }
    active_frontends.clear();
    frontend_handle.reset();
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

    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(2);
    if (node_server) node_server->Shutdown(deadline);
    if (client_server) client_server->Shutdown(deadline);
    if (node_server) node_server->Wait();
    if (client_server) client_server->Wait();
    node_server.reset();
    client_server.reset();
    node_service.reset();
    client_service.reset();
    jobs.reset();      // cancels unfinished jobs
    placer.reset();    // cancels remote work and joins workers
    {
        std::lock_guard lock(clients_mu);
        clients.clear();
    }
    local_executor.reset();  // kills running sandboxes and joins its workers
    discovery.reset();
    team.reset();
    pins.reset();
    fs::remove(sock, ec);
    return 0;
}

} // namespace zeromesh::node
