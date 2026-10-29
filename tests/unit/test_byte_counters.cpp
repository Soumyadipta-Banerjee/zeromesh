#include "tests/unit/node_fixture.hpp"

#include "node/convert.hpp"
#include "sched/placer.hpp"

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace v1 = zeromesh::v1;
using zeromesh::testing::TempDir;
using zeromesh::testing::TestNodeFull;

namespace {

void Expect(bool cond, const std::string& what) {
    if (!cond) throw std::runtime_error(what);
}

using Requester = zeromesh::testing::PlacerRequester;

// One batch per case; every case reads the shared tree plus its own `case.txt`.
std::vector<v1::BatchSpec> Sweep(const std::vector<v1::Digest>& roots) {
    std::vector<v1::BatchSpec> out;
    for (size_t i = 0; i < roots.size(); ++i) {
        out.push_back(zeromesh::testing::ShBatch({{"case" + std::to_string(i), "cat case.txt shared.bin | wc -c"}}, roots[i]));
    }
    return out;
}

std::string Blob(size_t n, char c) { return std::string(n, c); }

} // namespace

TEST(NodeClientReportsBytesOnTheWire) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);

    TempDir src;
    zeromesh::testing::WriteFile(src / "in.bin", Blob(100000, 'x'));
    auto root = a.merkle->BuildTree(src.path());
    std::vector<v1::Digest> digests;
    uint64_t expected = 0;
    for (const auto& d : a.merkle->CollectTree(root)) {
        digests.push_back(zeromesh::node::ToProto(d));
        expected += static_cast<uint64_t>(d.size_bytes);
    }

    auto client = a.ClientTo(b);
    std::string err;
    uint64_t pushed = 0;
    Expect(client->Push(*a.cas, digests, &err, &pushed), err);
    ASSERT_EQ(pushed, expected);

    // Pulling into a fresh store reports the same payload size.
    TempDir other;
    zeromesh::store::CasStore fresh(other / "cas");
    uint64_t pulled = 0;
    Expect(client->Pull(fresh, digests, &err, &pulled), err);
    ASSERT_EQ(pulled, expected);
}

TEST(PlacerSendsSharedInputOncePerDonor) {
    auto key = zeromesh::net::GenerateTeamKey();
    TestNodeFull a("node-a", key), b("node-b", key);
    a.Start();
    b.Start();
    a.Admit(b);
    a.peers->Upsert(b.AsPeer());
    Requester req(a);

    constexpr int kCases = 6;
    TempDir src;
    zeromesh::testing::WriteFile(src / "shared.bin", Blob(200000, 's'));
    std::vector<v1::Digest> roots;
    uint64_t one_tree = 0;
    for (int i = 0; i < kCases; ++i) {
        zeromesh::testing::WriteFile(src / "case.txt", "case " + std::to_string(i) + "\n");
        auto root = a.merkle->BuildTree(src.path());
        roots.push_back(zeromesh::node::ToProto(root));
    }
    // What a donor needs for all cases together: each distinct blob once.
    std::set<std::string> seen;
    for (const auto& r : roots) {
        for (const auto& d : a.merkle->CollectTree(zeromesh::node::FromProto(r))) {
            if (seen.insert(d.hash).second) one_tree += static_cast<uint64_t>(d.size_bytes);
        }
    }

    for (const auto& b : Sweep(roots)) req.Execute(b);
    req.WaitAll();

    const auto traffic = req.placer->Traffic();
    ASSERT_EQ(traffic.count("node-b"), static_cast<size_t>(1));
    const uint64_t up = traffic.at("node-b").bytes_pushed;
    // N cases sharing a 200 kB input must not cost N x 200 kB.
    Expect(up == one_tree, "uploaded " + std::to_string(up) + " bytes, a deduplicated sweep needs " +
                               std::to_string(one_tree));
}

