#include "tests/test_framework.hpp"
#include "cas/p2p_syncer.hpp"
#include "cas/store.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <filesystem>
#include <thread>

TEST(P2PSyncer_LocalHit) {
    using namespace zeromesh::cas;

    auto test_cas = std::filesystem::temp_directory_path() / "zm_test_syncer_local";
    std::filesystem::remove_all(test_cas);

    auto store = std::make_shared<CasStore>(test_cas);
    P2PSyncer syncer(store);

    std::string sample = "int compute() { return 42; }";
    auto digest = store->PutBlob(sample);

    // Should return from local CAS immediately
    auto result = syncer.EnsureBlob(digest, {});
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, sample);
    ASSERT_EQ(syncer.GetChunksSynced(), 0);

    std::filesystem::remove_all(test_cas);
}

TEST(P2PSyncer_MissingBlobFallback) {
    using namespace zeromesh::cas;

    auto test_cas = std::filesystem::temp_directory_path() / "zm_test_syncer_missing";
    std::filesystem::remove_all(test_cas);

    auto store = std::make_shared<CasStore>(test_cas);
    P2PSyncer syncer(store);

    Digest missing_digest{"deadbeef00000000000000000000000000000000000000000000000000000000", 100};
    auto result = syncer.EnsureBlob(missing_digest, {});
    ASSERT_FALSE(result.has_value());

    std::filesystem::remove_all(test_cas);
}

TEST(P2PSyncer_P2PNetworkReplication) {
    using namespace zeromesh::cas;

    auto test_cas = std::filesystem::temp_directory_path() / "zm_test_syncer_p2p";
    std::filesystem::remove_all(test_cas);

    auto store = std::make_shared<CasStore>(test_cas);
    P2PSyncer syncer(store);

    std::string remote_payload = "class NetworkSyncerPayload { int x; };";
    auto remote_digest = CasStore::ComputeDigest(remote_payload);

    // Spin up an ephemeral loopback TCP server to simulate a donor peer holding the blob
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(server_sock >= 0);

    int reuse = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = 0; // dynamic ephemeral port
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    ASSERT_EQ(bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)), 0);
    ASSERT_EQ(listen(server_sock, 1), 0);

    socklen_t addr_len = sizeof(server_addr);
    ASSERT_EQ(getsockname(server_sock, (struct sockaddr*)&server_addr, &addr_len), 0);
    int assigned_port = ntohs(server_addr.sin_port);

    std::atomic<bool> server_ready{true};
    std::thread server_thread([&]() {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
        if (client_sock >= 0) {
            char req_buf[256] = {0};
            read(client_sock, req_buf, sizeof(req_buf) - 1);
            // Reply with the remote payload
            send(client_sock, remote_payload.data(), remote_payload.size(), 0);
            close(client_sock);
        }
        close(server_sock);
    });

    std::vector<std::string> peers = {"127.0.0.1:" + std::to_string(assigned_port)};

    // Syncer pulls missing blob from the peer
    auto result = syncer.EnsureBlob(remote_digest, peers);

    server_thread.join();

    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, remote_payload);
    ASSERT_EQ(syncer.GetChunksSynced(), 1);
    ASSERT_EQ(syncer.GetTotalBytesReceived(), remote_payload.size());

    // Verified: blob has been deposited into local CAS store
    ASSERT_TRUE(store->HasBlob(remote_digest.hash));

    std::filesystem::remove_all(test_cas);
}
