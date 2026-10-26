#include "tests/test_framework.hpp"
#include "verify/byzantine_spot_checker.hpp"

TEST(ByzantineSpotChecker_SamplingRatio) {
    using namespace zeromesh::verify;

    // 100% ratio must always trigger spot checks
    ByzantineSpotChecker checker_all(1.0);
    ASSERT_TRUE(checker_all.ShouldSpotCheck("action-alpha"));
    ASSERT_TRUE(checker_all.ShouldSpotCheck("action-beta"));

    // 0% ratio must never trigger spot checks
    ByzantineSpotChecker checker_none(0.0);
    ASSERT_FALSE(checker_none.ShouldSpotCheck("action-alpha"));
    ASSERT_FALSE(checker_none.ShouldSpotCheck("action-beta"));

    // Intermediate ratio must be strictly deterministic across calls
    ByzantineSpotChecker checker_partial(0.5);
    bool check1 = checker_partial.ShouldSpotCheck("action-hash-xyz-12345");
    bool check2 = checker_partial.ShouldSpotCheck("action-hash-xyz-12345");
    ASSERT_EQ(check1, check2);
}

TEST(ByzantineSpotChecker_ResultVerification) {
    using namespace zeromesh::verify;

    ByzantineSpotChecker checker;

    // Identical outputs -> Matches
    ASSERT_EQ(
        checker.VerifyResult("action-1", "hash-out-abc", "hash-out-abc"),
        SpotCheckVerdict::Matches
    );

    // Divergent outputs -> Diverged (detects non-determinism or poisoned build)
    ASSERT_EQ(
        checker.VerifyResult("action-1", "hash-out-abc", "hash-out-corrupted"),
        SpotCheckVerdict::Diverged
    );

    // Missing output from shadow or primary node -> Timeout
    ASSERT_EQ(
        checker.VerifyResult("action-1", "hash-out-abc", ""),
        SpotCheckVerdict::Timeout
    );
    ASSERT_EQ(
        checker.VerifyResult("action-1", "", "hash-out-abc"),
        SpotCheckVerdict::Timeout
    );
}

TEST(ByzantineSpotChecker_QuarantineBlacklist) {
    using namespace zeromesh::verify;

    ByzantineSpotChecker checker;

    std::string rogue_peer = "rogue-worker-99";
    ASSERT_FALSE(checker.IsQuarantined(rogue_peer));

    checker.QuarantinePeer(rogue_peer);
    ASSERT_TRUE(checker.IsQuarantined(rogue_peer));

    // Other peers remain unaffected
    ASSERT_FALSE(checker.IsQuarantined("honest-worker-01"));
}

TEST(ByzantineSpotChecker_SignAndVerifyManifest) {
    using namespace zeromesh::verify;

    ByzantineSpotChecker checker;

    std::string manifest = "action:123;outputs:abc.o;exit:0";
    std::string sig = checker.SignManifest(manifest);

    ASSERT_FALSE(sig.empty());
    ASSERT_TRUE(checker.VerifySignature(manifest, sig, "dummy_pubkey"));

    // Tampered manifest must fail verification
    std::string tampered_manifest = "action:123;outputs:poisoned.o;exit:0";
    ASSERT_FALSE(checker.VerifySignature(tampered_manifest, sig, "dummy_pubkey"));
}
