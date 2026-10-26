#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zeromesh::verify {

/**
 * @brief Output verdict from a Byzantine verification check.
 */
enum class SpotCheckVerdict {
    Matches,
    Diverged,
    Timeout,
    MaliciousPayloadDetected
};

/**
 * @brief Zero-Trust & Byzantine Spot-Check Quorum Engine.
 * Randomly triggers a 2% shadow re-execution of completed tasks on independent peers.
 * Cross-validates cryptographic output digests and automatically quarantines
 * nodes producing divergent results to prevent cache poisoning.
 */
class ByzantineSpotChecker {
public:
    explicit ByzantineSpotChecker(double spot_check_ratio = 0.02); // 2% default
    ~ByzantineSpotChecker();

    /**
     * @brief Evaluates whether a given task should undergo shadow verification.
     */
    [[nodiscard]] bool ShouldSpotCheck(std::string_view action_digest) const;

    /**
     * @brief Dispatches a shadow re-execution to an independent node and verifies outputs.
     */
    SpotCheckVerdict VerifyResult(std::string_view action_digest,
                                  std::string_view primary_output_digest,
                                  std::string_view shadow_output_digest);

    /**
     * @brief Signs an action result manifest using host hardware root of trust (TPM / Enclave).
     */
    std::string SignManifest(std::string_view manifest_bytes);

    /**
     * @brief Verifies cryptographic signature on an action result manifest.
     */
    bool VerifySignature(std::string_view manifest_bytes,
                         std::string_view signature,
                         std::string_view peer_public_key);

    /**
     * @brief Blacklists a peer detected generating poisoned cache entries.
     */
    void QuarantinePeer(std::string_view peer_node_id);

    /**
     * @brief Checks if a peer is currently quarantined.
     */
    [[nodiscard]] bool IsQuarantined(std::string_view peer_node_id) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace zeromesh::verify
