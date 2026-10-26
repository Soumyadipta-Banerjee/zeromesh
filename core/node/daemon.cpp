#include "node/daemon.hpp"

#include "plugins/env-host/env_host.hpp"
#include "exec/sandbox_runner.hpp"
#include "net/peer_table.hpp"
#include "node/action_cache.hpp"
#include "node/client_service.hpp"
#include "node/job_manager.hpp"
#include "node/local_executor.hpp"
#include "node/state.hpp"
#include "plugin/loader.hpp"
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
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

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

    std::unique_ptr<grpc::Server> server;
    std::shared_ptr<JobManager> jobs;
    std::shared_ptr<LocalExecutor> executor;
    std::unique_ptr<ClientService> service;
    try {
        auto state = NodeState::LoadOrCreate(config_.state_dir);
        auto cas = std::make_shared<store::CasStore>(config_.state_dir / "cas");
        auto merkle = std::make_shared<store::MerkleBuilder>(cas);
        auto runner = std::make_shared<exec::SandboxRunner>(cas, merkle, config_.state_dir / "scratch");
        auto registry = std::make_shared<plugin::Registry>();
        registry->RegisterEnv(envhost::Make());
        registry->LoadSearchPath();
        int hw = static_cast<int>(std::thread::hardware_concurrency());
        int slots = std::max(1, hw - 1);
        executor = std::make_shared<LocalExecutor>(runner, registry, state.node_id(), slots);
        auto cache = std::make_shared<ActionCache>(cas);
        jobs = std::make_shared<JobManager>(cas, merkle, cache, executor);
        auto peers = std::make_shared<net::PeerTable>();

        ClientServiceOptions opts;
        opts.node_id = state.node_id();
        opts.node_port = config_.node_port;
        opts.upload_limit_bytes = UploadLimitFromEnv();
        service = std::make_unique<ClientService>(cas, merkle, jobs, peers, opts);

        grpc::ServerBuilder builder;
        builder.AddListeningPort("unix:" + sock.string(), grpc::InsecureServerCredentials());
        builder.RegisterService(service.get());
        mode_t old_umask = umask(0177);  // the socket is created 0600, closing the chmod race
        server = builder.BuildAndStart();
        umask(old_umask);
        if (!server) throw std::runtime_error("cannot listen on " + sock.string());
        if (chmod(sock.c_str(), 0600) != 0) {
            throw std::runtime_error("cannot chmod " + sock.string() + ": " + std::strerror(errno));
        }
        std::cerr << "zmd: node " << state.node_id() << " listening on " << sock.string() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "zmd: " << e.what() << "\n";
        if (server) server->Shutdown();
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

    // Running WatchJob streams end when their contexts are cancelled at the deadline.
    server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    server->Wait();
    server.reset();
    service.reset();
    jobs.reset();      // cancels unfinished jobs
    executor.reset();  // kills running sandboxes and joins its workers
    fs::remove(sock, ec);
    return 0;
}

} // namespace zeromesh::node
