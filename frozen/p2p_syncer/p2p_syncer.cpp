#include "cas/p2p_syncer.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>

namespace zeromesh::cas {

P2PSyncer::P2PSyncer(std::shared_ptr<CasStore> store)
    : store_(std::move(store)) {}

std::optional<std::string> P2PSyncer::EnsureBlob(
    const Digest& digest,
    const std::vector<std::string>& peer_endpoints
) {
    if (digest.hash.empty()) {
        return "";
    }

    // 1. Fast path: check local CAS store
    if (store_ && store_->HasBlob(digest.hash)) {
        return store_->GetBlob(digest.hash);
    }

    // 2. Slow path: synchronize from available LAN peers
    std::lock_guard<std::mutex> lock(sync_mutex_);

    // Double-check local cache under mutex
    if (store_ && store_->HasBlob(digest.hash)) {
        return store_->GetBlob(digest.hash);
    }

    for (const auto& endpoint : peer_endpoints) {
        size_t colon_pos = endpoint.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string host = endpoint.substr(0, colon_pos);
        int port = 0;
        try {
            port = std::stoi(endpoint.substr(colon_pos + 1));
        } catch (...) {
            continue;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        // Set non-blocking connect timeout (500ms)
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(static_cast<uint16_t>(port));

        if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
            close(sock);
            continue;
        }

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(sock);
            continue;
        }

        // Send binary wire query: "GET_CAS <hash>\n"
        std::string query = "GET_CAS " + digest.hash + "\n";
        if (send(sock, query.data(), query.size(), 0) <= 0) {
            close(sock);
            continue;
        }

        // Receive response
        std::string response;
        char buffer[4096];
        ssize_t bytes_read = 0;
        while ((bytes_read = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
            response.append(buffer, static_cast<size_t>(bytes_read));
            if (static_cast<int64_t>(response.size()) >= digest.size_bytes) break;
        }
        close(sock);

        if (!response.empty()) {
            auto computed = CasStore::ComputeDigest(response);
            if (computed.hash == digest.hash) {
                if (store_) {
                    store_->PutBlob(response);
                }
                bytes_received_ += response.size();
                chunks_synced_++;
                return response;
            }
        }
    }

    return std::nullopt;
}

void P2PSyncer::BroadcastBlobAvailability(const Digest& digest) {
    (void)digest;
    // Broadcast notification or DHT chunk provider announcement
}

} // namespace zeromesh::cas
