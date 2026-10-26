#include "net/discovery.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#include <iostream>
#include <random>
#include <sstream>
#include <vector>

namespace zeromesh::net {

Discovery::Discovery(std::shared_ptr<PeerTable> peer_table, DiscoveryConfig config)
    : peer_table_(std::move(peer_table)), config_(std::move(config)) {
    char hostname_buf[256] = {0};
    if (gethostname(hostname_buf, sizeof(hostname_buf) - 1) == 0) {
        if (config_.hostname.empty()) {
            config_.hostname = hostname_buf;
        }
    } else if (config_.hostname.empty()) {
        config_.hostname = "localhost";
    }

    if (config_.node_id.empty()) {
        std::random_device rd;
        config_.node_id = config_.hostname + "-" + std::to_string(getpid()) + "-" + std::to_string(rd() % 100000);
    }
}

Discovery::~Discovery() {
    Stop();
}

bool Discovery::Start() {
    if (running_.load()) {
        return true;
    }

    running_ = true;
    listener_thread_ = std::thread(&Discovery::ListenerLoop, this);
    advertiser_thread_ = std::thread(&Discovery::AdvertiserLoop, this);
    return true;
}

void Discovery::Stop() {
    if (!running_.load()) {
        return;
    }

    running_ = false;

    if (advertiser_thread_.joinable()) {
        advertiser_thread_.join();
    }
    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
}

void Discovery::AnnounceNow() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    uint8_t ttl = 4;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    uint8_t loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(static_cast<uint16_t>(config_.multicast_port));
    dest_addr.sin_addr.s_addr = inet_addr(config_.multicast_group.c_str());

    Telemetry t;
    if (config_.telemetry) {
        std::lock_guard<std::mutex> lock(telemetry_mutex_);
        t = config_.telemetry();
    } else {
        t.cores_total = config_.cores;
        t.cores_free = config_.cores;
        t.mem_total = config_.ram_bytes;
        t.mem_available = config_.ram_bytes;
    }

    // Fields: id hostname grpc_port cores mem_total load battery on_ac
    //         arch node_port cores_free mem_available
    std::ostringstream ss;
    ss << "ZM_ANNOUNCE\t"
       << config_.node_id << "\t"
       << config_.hostname << "\t"
       << config_.grpc_port << "\t"
       << t.cores_total << "\t"
       << t.mem_total << "\t"
       << t.cpu_load_pct << "\t"
       << t.battery_pct << "\t"
       << (t.on_ac ? "1" : "0") << "\t"
       << (t.arch.empty() ? "unknown" : t.arch) << "\t"
       << config_.node_port << "\t"
       << t.cores_free << "\t"
       << t.mem_available;

    std::string msg = ss.str();
    sendto(sock, msg.data(), msg.size(), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    close(sock);
}

void Discovery::BroadcastNodeDraining(const std::string& reason) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    uint8_t ttl = 4;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    uint8_t loop = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(static_cast<uint16_t>(config_.multicast_port));
    dest_addr.sin_addr.s_addr = inet_addr(config_.multicast_group.c_str());

    std::string msg = "ZM_DRAIN\t" + config_.node_id + "\t" + reason;
    sendto(sock, msg.data(), msg.size(), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    close(sock);
}

void Discovery::AdvertiserLoop() {
    while (running_.load()) {
        AnnounceNow();
        if (peer_table_) {
            peer_table_->PruneStale();
        }

        // Sleep in 100ms intervals to allow fast and clean termination
        for (int i = 0; i < 20 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void Discovery::ListenerLoop() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(static_cast<uint16_t>(config_.multicast_port));
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        close(sock);
        return;
    }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(config_.multicast_group.c_str());
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    char buffer[4096];
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;

    while (running_.load()) {
        int ret = poll(&pfd, 1, 200); // 200ms poll timeout
        if (ret <= 0) continue;

        sockaddr_in sender_addr{};
        socklen_t sender_len = sizeof(sender_addr);
        ssize_t bytes = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, (struct sockaddr*)&sender_addr, &sender_len);
        if (bytes <= 0) continue;
        buffer[bytes] = '\0';

        std::string packet(buffer, static_cast<size_t>(bytes));
        char sender_ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &(sender_addr.sin_addr), sender_ip, INET_ADDRSTRLEN);

        std::vector<std::string> tokens;
        size_t start = 0;
        while (start < packet.size()) {
            size_t tab = packet.find('\t', start);
            if (tab == std::string::npos) {
                tokens.push_back(packet.substr(start));
                break;
            }
            tokens.push_back(packet.substr(start, tab - start));
            start = tab + 1;
        }

        if (tokens.empty()) continue;

        if (tokens[0] == "ZM_ANNOUNCE" && tokens.size() >= 9) {
            std::string peer_id = tokens[1];
            if (peer_id == config_.node_id) {
                // Ignore own announcement
                continue;
            }

            Peer peer;
            try {
                peer.id = peer_id;
                peer.hostname = tokens[2];
                peer.ip_address = sender_ip;
                peer.grpc_port = std::stoi(tokens[3]);
                peer.cores = std::stoi(tokens[4]);
                peer.memory_bytes = std::stoull(tokens[5]);
                peer.cpu_load_pct = std::stod(tokens[6]);
                peer.battery_pct = std::stoi(tokens[7]);
                peer.is_charging = (tokens[8] == "1");
                if (tokens.size() >= 13) {
                    peer.arch = tokens[9];
                    peer.node_port = std::stoi(tokens[10]);
                    peer.cores_free = std::stoi(tokens[11]);
                    peer.mem_available = std::stoull(tokens[12]);
                }
            } catch (const std::exception&) {
                continue;  // malformed announcement from the LAN: drop it
            }
            peer.state = PeerState::Active;
            peer.latency = std::chrono::milliseconds(1);
            peer.last_seen = std::chrono::steady_clock::now();

            if (peer_table_) {
                peer_table_->Upsert(std::move(peer));
            }
        } else if (tokens[0] == "ZM_DRAIN" && tokens.size() >= 3) {
            std::string peer_id = tokens[1];
            std::string reason = tokens[2];
            if (peer_id != config_.node_id && peer_table_) {
                peer_table_->MarkDraining(peer_id, reason);
            }
        }
    }

    close(sock);
}

} // namespace zeromesh::net
