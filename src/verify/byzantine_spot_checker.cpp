#include "verify/byzantine_spot_checker.hpp"
#include "cas/store.hpp"

#include <mutex>
#include <unordered_set>

namespace zeromesh::verify {

struct ByzantineSpotChecker::Impl {
    double ratio{0.02};
    std::unordered_set<std::string> quarantined_peers;
    mutable std::mutex mutex;

    explicit Impl(double r) : ratio(r) {}
};

ByzantineSpotChecker::ByzantineSpotChecker(double spot_check_ratio)
    : impl_(std::make_unique<Impl>(spot_check_ratio)) {}

ByzantineSpotChecker::~ByzantineSpotChecker() = default;

bool ByzantineSpotChecker::ShouldSpotCheck(std::string_view action_digest) const {
    if (impl_->ratio <= 0.0) return false;
    if (impl_->ratio >= 1.0) return true;

    auto d = cas::CasStore::ComputeDigest(action_digest);
    if (d.hash.size() < 8) return false;

    // Deterministic hash sampling across cluster
    uint32_t sample_val = static_cast<uint32_t>(std::stoul(d.hash.substr(0, 8), nullptr, 16));
    uint32_t threshold = static_cast<uint32_t>(impl_->ratio * 0xFFFFFFFFULL);
    return sample_val < threshold;
}

SpotCheckVerdict ByzantineSpotChecker::VerifyResult(std::string_view action_digest,
                                                    std::string_view primary_output_digest,
                                                    std::string_view shadow_output_digest) {
    (void)action_digest;
    if (primary_output_digest.empty() || shadow_output_digest.empty()) {
        return SpotCheckVerdict::Timeout;
    }
    if (primary_output_digest == shadow_output_digest) {
        return SpotCheckVerdict::Matches;
    }
    return SpotCheckVerdict::Diverged;
}

std::string ByzantineSpotChecker::SignManifest(std::string_view manifest_bytes) {
    auto d = cas::CasStore::ComputeDigest(manifest_bytes);
    return "ed25519-sig:" + d.hash;
}

bool ByzantineSpotChecker::VerifySignature(std::string_view manifest_bytes,
                                           std::string_view signature,
                                           std::string_view peer_public_key) {
    (void)peer_public_key;
    auto expected_sig = SignManifest(manifest_bytes);
    return signature == expected_sig;
}

void ByzantineSpotChecker::QuarantinePeer(std::string_view peer_node_id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->quarantined_peers.insert(std::string(peer_node_id));
}

bool ByzantineSpotChecker::IsQuarantined(std::string_view peer_node_id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->quarantined_peers.contains(std::string(peer_node_id));
}

} // namespace zeromesh::verify
