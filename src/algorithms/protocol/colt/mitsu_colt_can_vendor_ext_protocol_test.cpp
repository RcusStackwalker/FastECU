#include <gtest/gtest.h>
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/testing/byte_test_utils.h"
using namespace MitsuColtCanVendorExt;
using test_bytes::bytesFromHex;

TEST(TestMitsuColtCanVendorExtProtocol, challenge_transform_matches_known_vectors)
{
    // Vectors confirmed by exhaustive brute-force cross-check against
    // challengeInverseTransform() across the full 32-bit domain — 0
    // mismatches out of 4294967296. See design doc "Pre-verified facts".
    ASSERT_EQ(challengeTransform(0x00000000U), std::uint32_t(0xF2E207C5U));
    ASSERT_EQ(challengeTransform(0xFFFFFFFFU), std::uint32_t(0xE4C2C64DU));
    ASSERT_EQ(challengeTransform(0x12345678U), std::uint32_t(0x669E0CB4U));
    ASSERT_EQ(challengeTransform(0x00000001U), std::uint32_t(0xE0E207D2U));
    ASSERT_EQ(challengeTransform(0xDEADBEEFU), std::uint32_t(0xA5654127U));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_inverse_transform_matches_known_vectors)
{
    // Same five vectors as the forward test, inverted.
    ASSERT_EQ(challengeInverseTransform(0xF2E207C5U), std::uint32_t(0x00000000U));
    ASSERT_EQ(challengeInverseTransform(0xE4C2C64DU), std::uint32_t(0xFFFFFFFFU));
    ASSERT_EQ(challengeInverseTransform(0x669E0CB4U), std::uint32_t(0x12345678U));
    ASSERT_EQ(challengeInverseTransform(0xE0E207D2U), std::uint32_t(0x00000001U));
    ASSERT_EQ(challengeInverseTransform(0xA5654127U), std::uint32_t(0xDEADBEEFU));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_inverse_round_trips_with_forward)
{
    // Lightweight regression check standing in for the one-time
    // exhaustive 2^32 proof recorded in the design doc.
    const std::uint32_t values[] = {0x00000000U, 0xFFFFFFFFU, 0x12345678U, 0x00000001U,
                                    0xDEADBEEFU, 0x80000000U, 0x7FFFFFFFU};
    for (std::uint32_t x : values)
    {
        ASSERT_EQ(challengeInverseTransform(challengeTransform(x)), x);
    }
}
TEST(TestMitsuColtCanVendorExtProtocol, byte_native_seed_and_key_helpers_round_trip)
{
    const bytes::Bytes seedBytes = bytesFromHex("F2E207C5");
    ASSERT_EQ(bytesToSeed(seedBytes), std::uint32_t(0xF2E207C5U));
    ASSERT_EQ(keyBytes(0xF2E207C5U), seedBytes);

    const std::uint32_t secret = 0x12345678U;
    const bytes::Bytes onWire = keyBytes(challengeTransform(secret));
    ASSERT_EQ(challengeInverseTransform(bytesToSeed(onWire)), secret);
}
TEST(TestMitsuColtCanVendorExtProtocol, byte_native_challenge_frame_layout)
{
    ASSERT_EQ(buildChallengeSeedRequest(), bytesFromHex("232741"));
    ASSERT_EQ(buildChallengeKey(0x12345678U), bytesFromHex("23274212345678"));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_key_frame_uses_inverse_key_bytes)
{
    const auto seedBytes = bytesFromHex("669E0CB4");
    const std::uint32_t key = challengeInverseTransform(bytesToSeed(seedBytes));
    ASSERT_EQ(keyBytes(key), bytesFromHex("12345678"));
    ASSERT_EQ(buildChallengeKey(key), bytesFromHex("23274212345678"));
}
