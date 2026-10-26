#include "dht/kademlia.hpp"
#include "dht/local_store.hpp"
#include "dht/routing_table.hpp"
#include "dht/transport.hpp"
#include "dht/types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <unordered_map>

namespace zeromesh::dht {

// ─────────────────────────────────────────────────────────────────────────────
// KademliaDht::Impl — PIMPL internals
// ─────────────────────────────────────────────────────────────────────────────

struct KademliaDht::Impl {
    NodeId self_id;
    uint16_t listen_port;

    RoutingTable routing_table;
    LocalStore local_store;
    UdpTransport transport;

    std::atomic<bool> running{false};
    std::thread maintenance_thread;

    // ── Pending RPC tracking for async request/response correlation ──

    struct PendingRpc {
        RpcType expected_type{};
        std::chrono::steady_clock::time_point sent_at;
        std::function<void(const RpcMessage&)> callback;
    };

    std::mutex pending_mutex;
    std::unordered_map<TransactionId, PendingRpc> pending_rpcs;
    std::atomic<TransactionId> next_txn_id{1};

    // ── Construction ────────────────────────────────────────────────────

    Impl(NodeId id, uint16_t port)
        : self_id(id)
        , listen_port(port)
        , routing_table(id)
        , transport(TransportConfig{"0.0.0.0", port}) {}

    // ── Transaction Management ──────────────────────────────────────────

    TransactionId AllocateTxnId() {
        return next_txn_id.fetch_add(1);
    }

    RpcHeader MakeHeader(RpcType type) {
        RpcHeader h;
        h.type = type;
        h.txn_id = AllocateTxnId();
        h.sender_id = self_id;
        return h;
    }

    void RegisterPending(TransactionId txn, RpcType expected_reply,
                         std::function<void(const RpcMessage&)> cb) {
        std::lock_guard<std::mutex> lock(pending_mutex);
        PendingRpc rpc;
        rpc.expected_type = expected_reply;
        rpc.sent_at = std::chrono::steady_clock::now();
        rpc.callback = std::move(cb);
        pending_rpcs[txn] = std::move(rpc);
    }

    void ResolvePending(TransactionId txn, const RpcMessage& response) {
        std::function<void(const RpcMessage&)> cb;
        {
            std::lock_guard<std::mutex> lock(pending_mutex);
            auto it = pending_rpcs.find(txn);
            if (it == pending_rpcs.end()) return;
            cb = std::move(it->second.callback);
            pending_rpcs.erase(it);
        }
        if (cb) cb(response);
    }

    void PurgeTimedOutRpcs() {
        std::lock_guard<std::mutex> lock(pending_mutex);
        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_rpcs.begin(); it != pending_rpcs.end(); ) {
            if (now - it->second.sent_at > kRpcTimeout) {
                it = pending_rpcs.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ── Inbound Message Dispatch ────────────────────────────────────────

    void OnMessage(const std::string& sender_addr, const RpcMessage& message) {
        std::visit([&](const auto& msg) {
            using T = std::decay_t<decltype(msg)>;

            // Always refresh the sender in our routing table.
            RefreshSenderInTable(msg.header.sender_id, sender_addr);

            if constexpr (std::is_same_v<T, PingRequest>) {
                HandlePing(sender_addr, msg);
            }
            else if constexpr (std::is_same_v<T, FindNodeRequest>) {
                HandleFindNode(sender_addr, msg);
            }
            else if constexpr (std::is_same_v<T, FindValueRequest>) {
                HandleFindValue(sender_addr, msg);
            }
            else if constexpr (std::is_same_v<T, StoreRequest>) {
                HandleStore(sender_addr, msg);
            }
            else if constexpr (std::is_same_v<T, AnnounceChunkRequest>) {
                HandleAnnounceChunk(sender_addr, msg);
            }
            else if constexpr (std::is_same_v<T, GetProvidersRequest>) {
                HandleGetProviders(sender_addr, msg);
            }
            else {
                // Response types: dispatch to pending callback.
                ResolvePending(msg.header.txn_id, message);
            }
        }, message);
    }

    void RefreshSenderInTable(const NodeId& sender_id, const std::string& addr) {
        // Parse "ip:port"
        auto colon = addr.rfind(':');
        if (colon == std::string::npos) return;

        DhtNode node;
        node.id = sender_id;
        node.ip_address = addr.substr(0, colon);
        node.port = static_cast<uint16_t>(std::stoi(addr.substr(colon + 1)));
        node.last_seen = std::chrono::steady_clock::now();
        routing_table.InsertOrRefresh(node);
    }

    // ── Request Handlers ────────────────────────────────────────────────

    void HandlePing(const std::string& sender_addr, const PingRequest& req) {
        PongResponse resp;
        resp.header.type = RpcType::Pong;
        resp.header.txn_id = req.header.txn_id;
        resp.header.sender_id = self_id;
        transport.SendMessage(sender_addr, resp);
    }

    void HandleFindNode(const std::string& sender_addr, const FindNodeRequest& req) {
        auto closest = routing_table.FindClosest(req.target, kBucketSize);

        FindNodeResponse resp;
        resp.header.type = RpcType::FindNodeReply;
        resp.header.txn_id = req.header.txn_id;
        resp.header.sender_id = self_id;
        for (const auto& n : closest) {
            resp.closest_nodes.push_back(PeerContact{n.id, n.ip_address, n.port});
        }
        transport.SendMessage(sender_addr, resp);
    }

    void HandleFindValue(const std::string& sender_addr, const FindValueRequest& req) {
        FindValueResponse resp;
        resp.header.type = RpcType::FindValueReply;
        resp.header.txn_id = req.header.txn_id;
        resp.header.sender_id = self_id;

        auto val = local_store.GetValue(req.key);
        if (val) {
            resp.value = std::move(*val);
        } else {
            auto closest = routing_table.FindClosest(req.key, kBucketSize);
            for (const auto& n : closest) {
                resp.closest_nodes.push_back(PeerContact{n.id, n.ip_address, n.port});
            }
        }
        transport.SendMessage(sender_addr, resp);
    }

    void HandleStore(const std::string& sender_addr, const StoreRequest& req) {
        bool ok = local_store.PutValue(req.key, req.value,
                                        req.header.sender_id, req.ttl);
        StoreResponse resp;
        resp.header.type = RpcType::StoreReply;
        resp.header.txn_id = req.header.txn_id;
        resp.header.sender_id = self_id;
        resp.success = ok;
        transport.SendMessage(sender_addr, resp);
    }

    void HandleAnnounceChunk(const std::string& sender_addr, const AnnounceChunkRequest& req) {
        // Extract sender IP from the transport address.
        auto colon = sender_addr.rfind(':');
        std::string ip = (colon != std::string::npos)
                             ? sender_addr.substr(0, colon)
                             : sender_addr;

        PeerContact provider;
        provider.id = req.header.sender_id;
        provider.ip_address = ip;
        provider.port = req.data_port;
        local_store.AddProvider(req.chunk_hash, provider);

        AnnounceChunkResponse resp;
        resp.header.type = RpcType::AnnounceReply;
        resp.header.txn_id = req.header.txn_id;
        resp.header.sender_id = self_id;
        resp.success = true;
        transport.SendMessage(sender_addr, resp);
    }

    void HandleGetProviders(const std::string& sender_addr, const GetProvidersRequest& req) {
        GetProvidersResponse resp;
        resp.header.type = RpcType::ProvidersReply;
        resp.header.txn_id = req.header.txn_id;
        resp.header.sender_id = self_id;
        resp.providers = local_store.GetProviders(req.chunk_hash);

        // Also return closest known nodes so the requester can continue
        // the iterative lookup if providers are insufficient.
        auto closest = routing_table.FindClosest(req.chunk_hash, kBucketSize);
        for (const auto& n : closest) {
            resp.closest_nodes.push_back(PeerContact{n.id, n.ip_address, n.port});
        }
        transport.SendMessage(sender_addr, resp);
    }

    // ── Iterative Lookup Engine (α-concurrent convergence) ──────────────

    /**
     * Performs an iterative FIND_NODE lookup converging on the target key.
     *
     * Algorithm:
     * 1. Seed with the α closest nodes from local routing table.
     * 2. Send parallel FIND_NODE requests.
     * 3. Merge returned contacts, re-sort by XOR distance.
     * 4. Repeat until no closer nodes are discovered in a round.
     */
    std::vector<DhtNode> IterativeFindNode(const Key& target) {
        auto seed_nodes = routing_table.FindClosest(target, kBucketSize);
        if (seed_nodes.empty()) return {};

        // Shortlist: sorted by distance to target, used to drive convergence.
        struct ShortlistEntry {
            DhtNode node;
            bool queried{false};
        };
        std::vector<ShortlistEntry> shortlist;
        for (auto& n : seed_nodes) {
            shortlist.push_back({n, false});
        }

        // Sort helper.
        auto sort_shortlist = [&]() {
            std::sort(shortlist.begin(), shortlist.end(),
                      [&target](const ShortlistEntry& a, const ShortlistEntry& b) {
                          Key da = KademliaDht::XorDistance(a.node.id, target);
                          Key db = KademliaDht::XorDistance(b.node.id, target);
                          return da < db;
                      });
        };

        sort_shortlist();

        // Track which node IDs we've already queried to avoid duplicates.
        auto node_id_seen = [&](const NodeId& id) -> bool {
            for (const auto& e : shortlist) {
                if (e.node.id == id) return true;
            }
            return false;
        };

        // Iterative rounds.
        for (int round = 0; round < 20; ++round) { // Safety cap.
            // Select up to α unqueried nodes from the shortlist.
            std::vector<ShortlistEntry*> batch;
            for (auto& entry : shortlist) {
                if (!entry.queried) {
                    batch.push_back(&entry);
                    if (batch.size() >= kAlpha) break;
                }
            }

            if (batch.empty()) break; // All queried; converged.

            // Send FIND_NODE to each and collect responses synchronously.
            // In a production async implementation, these would be parallel;
            // here we block per-peer for simplicity in the initial version.
            bool found_closer = false;

            for (auto* entry : batch) {
                entry->queried = true;
                std::string dest = entry->node.ip_address + ":" +
                                   std::to_string(entry->node.port);

                FindNodeRequest req;
                req.header = MakeHeader(RpcType::FindNode);
                req.target = target;

                // Synchronous wait with timeout using a condition variable.
                std::mutex cv_mutex;
                std::condition_variable cv;
                bool got_response = false;
                std::vector<PeerContact> returned_contacts;

                RegisterPending(req.header.txn_id, RpcType::FindNodeReply,
                    [&](const RpcMessage& resp) {
                        if (auto* fnr = std::get_if<FindNodeResponse>(&resp)) {
                            returned_contacts = fnr->closest_nodes;
                        }
                        std::lock_guard<std::mutex> lk(cv_mutex);
                        got_response = true;
                        cv.notify_one();
                    });

                transport.SendMessage(dest, req);

                {
                    std::unique_lock<std::mutex> lk(cv_mutex);
                    cv.wait_for(lk, kRpcTimeout, [&] { return got_response; });
                }

                if (!got_response) {
                    // Peer didn't respond; consider removing from routing table.
                    routing_table.Remove(entry->node.id);
                    continue;
                }

                // Merge newly discovered contacts into the shortlist.
                for (const auto& pc : returned_contacts) {
                    if (pc.id == self_id) continue; // Skip self.
                    if (node_id_seen(pc.id)) continue;

                    DhtNode new_node;
                    new_node.id = pc.id;
                    new_node.ip_address = pc.ip_address;
                    new_node.port = pc.port;
                    new_node.last_seen = std::chrono::steady_clock::now();

                    routing_table.InsertOrRefresh(new_node);
                    shortlist.push_back({new_node, false});
                    found_closer = true;
                }
            }

            sort_shortlist();

            // Trim shortlist to kBucketSize to bound memory.
            if (shortlist.size() > kBucketSize) {
                shortlist.resize(kBucketSize);
            }

            if (!found_closer) break; // Converged.
        }

        // Return the k closest nodes from the final shortlist.
        std::vector<DhtNode> result;
        for (const auto& entry : shortlist) {
            result.push_back(entry.node);
            if (result.size() >= kBucketSize) break;
        }
        return result;
    }

    /**
     * Iterative FIND_VALUE lookup: converges on the target key,
     * returning the stored value if any node along the path holds it.
     */
    std::optional<std::string> IterativeFindValue(const Key& key) {
        // Check local store first.
        auto local_val = local_store.GetValue(key);
        if (local_val) return local_val;

        auto seed_nodes = routing_table.FindClosest(key, kBucketSize);
        if (seed_nodes.empty()) return std::nullopt;

        struct ShortlistEntry {
            DhtNode node;
            bool queried{false};
        };
        std::vector<ShortlistEntry> shortlist;
        for (auto& n : seed_nodes) {
            shortlist.push_back({n, false});
        }

        auto sort_shortlist = [&]() {
            std::sort(shortlist.begin(), shortlist.end(),
                      [&key](const ShortlistEntry& a, const ShortlistEntry& b) {
                          return KademliaDht::XorDistance(a.node.id, key) <
                                 KademliaDht::XorDistance(b.node.id, key);
                      });
        };

        sort_shortlist();

        for (int round = 0; round < 20; ++round) {
            std::vector<ShortlistEntry*> batch;
            for (auto& entry : shortlist) {
                if (!entry.queried) {
                    batch.push_back(&entry);
                    if (batch.size() >= kAlpha) break;
                }
            }
            if (batch.empty()) break;

            for (auto* entry : batch) {
                entry->queried = true;
                std::string dest = entry->node.ip_address + ":" +
                                   std::to_string(entry->node.port);

                FindValueRequest req;
                req.header = MakeHeader(RpcType::FindValue);
                req.key = key;

                std::mutex cv_mutex;
                std::condition_variable cv;
                bool got_response = false;
                std::optional<std::string> found_value;
                std::vector<PeerContact> returned_contacts;

                RegisterPending(req.header.txn_id, RpcType::FindValueReply,
                    [&](const RpcMessage& resp) {
                        if (auto* fvr = std::get_if<FindValueResponse>(&resp)) {
                            found_value = fvr->value;
                            returned_contacts = fvr->closest_nodes;
                        }
                        std::lock_guard<std::mutex> lk(cv_mutex);
                        got_response = true;
                        cv.notify_one();
                    });

                transport.SendMessage(dest, req);

                {
                    std::unique_lock<std::mutex> lk(cv_mutex);
                    cv.wait_for(lk, kRpcTimeout, [&] { return got_response; });
                }

                if (!got_response) {
                    routing_table.Remove(entry->node.id);
                    continue;
                }

                // If the value was found, cache it locally and return.
                if (found_value) {
                    local_store.PutValue(key, *found_value, entry->node.id);
                    return found_value;
                }

                // Otherwise, merge closest nodes.
                for (const auto& pc : returned_contacts) {
                    if (pc.id == self_id) continue;
                    bool exists = false;
                    for (const auto& e : shortlist) {
                        if (e.node.id == pc.id) { exists = true; break; }
                    }
                    if (exists) continue;

                    DhtNode new_node;
                    new_node.id = pc.id;
                    new_node.ip_address = pc.ip_address;
                    new_node.port = pc.port;
                    new_node.last_seen = std::chrono::steady_clock::now();
                    routing_table.InsertOrRefresh(new_node);
                    shortlist.push_back({new_node, false});
                }
            }

            sort_shortlist();
            if (shortlist.size() > kBucketSize) {
                shortlist.resize(kBucketSize);
            }
        }

        return std::nullopt;
    }

    /**
     * Iterative GET_PROVIDERS lookup for CAS chunk discovery.
     */
    std::vector<DhtNode> IterativeGetProviders(const Key& chunk_hash) {
        // Check local provider store first.
        auto local_providers = local_store.GetProviders(chunk_hash);

        auto seed_nodes = routing_table.FindClosest(chunk_hash, kBucketSize);

        struct ShortlistEntry {
            DhtNode node;
            bool queried{false};
        };
        std::vector<ShortlistEntry> shortlist;
        for (auto& n : seed_nodes) {
            shortlist.push_back({n, false});
        }

        std::sort(shortlist.begin(), shortlist.end(),
                  [&chunk_hash](const ShortlistEntry& a, const ShortlistEntry& b) {
                      return KademliaDht::XorDistance(a.node.id, chunk_hash) <
                             KademliaDht::XorDistance(b.node.id, chunk_hash);
                  });

        // Accumulate provider contacts across the lookup.
        std::vector<PeerContact> all_providers(std::move(local_providers));

        for (int round = 0; round < 20; ++round) {
            std::vector<ShortlistEntry*> batch;
            for (auto& entry : shortlist) {
                if (!entry.queried) {
                    batch.push_back(&entry);
                    if (batch.size() >= kAlpha) break;
                }
            }
            if (batch.empty()) break;

            for (auto* entry : batch) {
                entry->queried = true;
                std::string dest = entry->node.ip_address + ":" +
                                   std::to_string(entry->node.port);

                GetProvidersRequest req;
                req.header = MakeHeader(RpcType::GetProviders);
                req.chunk_hash = chunk_hash;

                std::mutex cv_mutex;
                std::condition_variable cv;
                bool got_response = false;
                std::vector<PeerContact> resp_providers;
                std::vector<PeerContact> resp_closest;

                RegisterPending(req.header.txn_id, RpcType::ProvidersReply,
                    [&](const RpcMessage& resp) {
                        if (auto* gpr = std::get_if<GetProvidersResponse>(&resp)) {
                            resp_providers = gpr->providers;
                            resp_closest = gpr->closest_nodes;
                        }
                        std::lock_guard<std::mutex> lk(cv_mutex);
                        got_response = true;
                        cv.notify_one();
                    });

                transport.SendMessage(dest, req);

                {
                    std::unique_lock<std::mutex> lk(cv_mutex);
                    cv.wait_for(lk, kRpcTimeout, [&] { return got_response; });
                }

                if (!got_response) {
                    routing_table.Remove(entry->node.id);
                    continue;
                }

                // Accumulate providers.
                for (const auto& p : resp_providers) {
                    bool dup = false;
                    for (const auto& existing : all_providers) {
                        if (existing.id == p.id) { dup = true; break; }
                    }
                    if (!dup) all_providers.push_back(p);
                }

                // Merge closest nodes.
                for (const auto& pc : resp_closest) {
                    if (pc.id == self_id) continue;
                    bool exists = false;
                    for (const auto& e : shortlist) {
                        if (e.node.id == pc.id) { exists = true; break; }
                    }
                    if (exists) continue;

                    DhtNode new_node;
                    new_node.id = pc.id;
                    new_node.ip_address = pc.ip_address;
                    new_node.port = pc.port;
                    new_node.last_seen = std::chrono::steady_clock::now();
                    routing_table.InsertOrRefresh(new_node);
                    shortlist.push_back({new_node, false});
                }
            }

            std::sort(shortlist.begin(), shortlist.end(),
                      [&chunk_hash](const ShortlistEntry& a, const ShortlistEntry& b) {
                          return KademliaDht::XorDistance(a.node.id, chunk_hash) <
                                 KademliaDht::XorDistance(b.node.id, chunk_hash);
                      });
            if (shortlist.size() > kBucketSize) {
                shortlist.resize(kBucketSize);
            }
        }

        // Convert provider contacts to DhtNode results.
        std::vector<DhtNode> result;
        for (const auto& pc : all_providers) {
            DhtNode n;
            n.id = pc.id;
            n.ip_address = pc.ip_address;
            n.port = pc.port;
            n.last_seen = std::chrono::steady_clock::now();
            result.push_back(std::move(n));
        }
        return result;
    }

    // ── Maintenance Loop ────────────────────────────────────────────────

    void MaintenanceLoop() {
        while (running.load()) {
            // 1. Purge expired local store entries.
            local_store.PurgeExpired();

            // 2. Purge timed-out pending RPCs.
            PurgeTimedOutRpcs();

            // 3. Refresh stale k-buckets by performing a random FIND_NODE.
            auto stale = routing_table.StaleBuckets(kRefreshInterval);
            for (int bucket_idx : stale) {
                // Generate a random key in this bucket's range and look it up.
                Key random_target{};
                std::random_device rd;
                std::mt19937 gen(rd());
                for (auto& byte : random_target) {
                    byte = static_cast<uint8_t>(gen() & 0xFF);
                }
                IterativeFindNode(random_target);
                routing_table.TouchBucket(bucket_idx);
            }

            // 4. Republish values approaching TTL expiry.
            auto republish_entries = local_store.EntriesNeedingRepublish();
            for (const auto& entry : republish_entries) {
                // Find k closest nodes to the key and STORE on them.
                auto closest = IterativeFindNode(entry.key);
                for (const auto& node : closest) {
                    if (node.id == self_id) continue;

                    StoreRequest req;
                    req.header = MakeHeader(RpcType::Store);
                    req.key = entry.key;
                    req.value = entry.value;
                    auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
                        entry.expires_at - std::chrono::steady_clock::now());
                    req.ttl = (remaining.count() > 0) ? remaining : std::chrono::seconds(60);

                    std::string dest = node.ip_address + ":" + std::to_string(node.port);
                    transport.SendMessage(dest, req);
                }
            }

            // Sleep for 1 minute between maintenance cycles.
            for (int i = 0; i < 60 && running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// KademliaDht Public API
// ─────────────────────────────────────────────────────────────────────────────

KademliaDht::KademliaDht(NodeId self_id, uint16_t listen_port)
    : impl_(std::make_unique<Impl>(self_id, listen_port)) {}

KademliaDht::~KademliaDht() {
    Stop();
}

Key KademliaDht::XorDistance(const Key& a, const Key& b) {
    Key dist{};
    for (size_t i = 0; i < kKeyBytes; ++i) {
        dist[i] = a[i] ^ b[i];
    }
    return dist;
}

bool KademliaDht::Start() {
    if (impl_->running.load()) return false;

    // Start the UDP transport with our message handler.
    bool ok = impl_->transport.Start(
        [this](const std::string& sender_addr, const RpcMessage& msg) {
            impl_->OnMessage(sender_addr, msg);
        });

    if (!ok) return false;

    impl_->running.store(true);

    // Launch maintenance thread (bucket refresh, TTL purge, republish).
    impl_->maintenance_thread = std::thread(&Impl::MaintenanceLoop, impl_.get());

    // Bootstrap: broadcast a FIND_NODE for our own ID to seed k-buckets
    // from any peers already on the LAN (1-hop shortcut optimization).
    FindNodeRequest bootstrap_req;
    bootstrap_req.header = impl_->MakeHeader(RpcType::FindNode);
    bootstrap_req.target = impl_->self_id;
    impl_->transport.BroadcastMessage(bootstrap_req);

    return true;
}

void KademliaDht::Stop() {
    if (!impl_->running.exchange(false)) return;

    impl_->transport.Stop();
    if (impl_->maintenance_thread.joinable()) {
        impl_->maintenance_thread.join();
    }
}

void KademliaDht::InsertNode(const DhtNode& node) {
    impl_->routing_table.InsertOrRefresh(node);
}

std::vector<DhtNode> KademliaDht::FindClosestNodes(const Key& target, size_t count) {
    if (impl_->running.load()) {
        // Online: perform full iterative lookup for fresh results.
        return impl_->IterativeFindNode(target);
    }
    // Offline / not started: return from local routing table only.
    return impl_->routing_table.FindClosest(target, count);
}

void KademliaDht::AnnounceChunk(const Key& chunk_hash, uint16_t data_port) {
    uint16_t port_to_use = (data_port > 0) ? data_port : impl_->listen_port;

    // Register ourselves as a provider locally.
    PeerContact self_contact;
    self_contact.id = impl_->self_id;
    self_contact.ip_address = "127.0.0.1";
    self_contact.port = port_to_use;
    impl_->local_store.AddProvider(chunk_hash, self_contact);

    // Find the k closest nodes to the chunk hash and send ANNOUNCE_CHUNK.
    auto closest = impl_->IterativeFindNode(chunk_hash);
    for (const auto& node : closest) {
        if (node.id == impl_->self_id) continue;

        AnnounceChunkRequest req;
        req.header = impl_->MakeHeader(RpcType::AnnounceChunk);
        req.chunk_hash = chunk_hash;
        req.data_port = port_to_use;

        std::string dest = node.ip_address + ":" + std::to_string(node.port);
        impl_->transport.SendMessage(dest, req);
    }
}

std::vector<DhtNode> KademliaDht::GetChunkProviders(const Key& chunk_hash) {
    return impl_->IterativeGetProviders(chunk_hash);
}

void KademliaDht::StoreValue(const Key& key, const std::string& value) {
    // Store locally first.
    impl_->local_store.PutValue(key, value, impl_->self_id);

    // Replicate to the kReplicationFactor closest nodes.
    auto closest = impl_->IterativeFindNode(key);
    size_t stored_count = 0;
    for (const auto& node : closest) {
        if (node.id == impl_->self_id) continue;
        if (stored_count >= kReplicationFactor) break;

        StoreRequest req;
        req.header = impl_->MakeHeader(RpcType::Store);
        req.key = key;
        req.value = value;
        req.ttl = std::chrono::duration_cast<std::chrono::seconds>(kValueTtl);

        std::string dest = node.ip_address + ":" + std::to_string(node.port);
        impl_->transport.SendMessage(dest, req);
        ++stored_count;
    }
}

std::optional<std::string> KademliaDht::FindValue(const Key& key) {
    return impl_->IterativeFindValue(key);
}

} // namespace zeromesh::dht
