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
    ASSERT_EQ(challengeTransform(0x00000000u), std::uint32_t(0xF2E207C5u));
    ASSERT_EQ(challengeTransform(0xFFFFFFFFu), std::uint32_t(0xE4C2C64Du));
    ASSERT_EQ(challengeTransform(0x12345678u), std::uint32_t(0x669E0CB4u));
    ASSERT_EQ(challengeTransform(0x00000001u), std::uint32_t(0xE0E207D2u));
    ASSERT_EQ(challengeTransform(0xDEADBEEFu), std::uint32_t(0xA5654127u));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_inverse_transform_matches_known_vectors)
{
    // Same five vectors as the forward test, inverted.
    ASSERT_EQ(challengeInverseTransform(0xF2E207C5u), std::uint32_t(0x00000000u));
    ASSERT_EQ(challengeInverseTransform(0xE4C2C64Du), std::uint32_t(0xFFFFFFFFu));
    ASSERT_EQ(challengeInverseTransform(0x669E0CB4u), std::uint32_t(0x12345678u));
    ASSERT_EQ(challengeInverseTransform(0xE0E207D2u), std::uint32_t(0x00000001u));
    ASSERT_EQ(challengeInverseTransform(0xA5654127u), std::uint32_t(0xDEADBEEFu));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_inverse_round_trips_with_forward)
{
    // Lightweight regression check standing in for the one-time
    // exhaustive 2^32 proof recorded in the design doc.
    const std::uint32_t values[] = {0x00000000u, 0xFFFFFFFFu, 0x12345678u, 0x00000001u,
                                    0xDEADBEEFu, 0x80000000u, 0x7FFFFFFFu};
    for (std::uint32_t x : values)
    {
        ASSERT_EQ(challengeInverseTransform(challengeTransform(x)), x);
    }
}
TEST(TestMitsuColtCanVendorExtProtocol, byte_native_seed_and_key_helpers_round_trip)
{
    const bytes::Bytes seedBytes = bytesFromHex("F2E207C5");
    ASSERT_EQ(bytesToSeed(seedBytes), std::uint32_t(0xF2E207C5u));
    ASSERT_EQ(keyBytes(0xF2E207C5u), seedBytes);

    const std::uint32_t secret = 0x12345678u;
    const bytes::Bytes onWire = keyBytes(challengeTransform(secret));
    ASSERT_EQ(challengeInverseTransform(bytesToSeed(onWire)), secret);
}
TEST(TestMitsuColtCanVendorExtProtocol, byte_native_challenge_frame_layout)
{
    ASSERT_EQ(buildChallengeSeedRequest(), bytesFromHex("232741"));
    ASSERT_EQ(buildChallengeKey(0x12345678u), bytesFromHex("23274212345678"));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_key_frame_uses_inverse_key_bytes)
{
    const auto seedBytes = bytesFromHex("669E0CB4");
    const std::uint32_t key = challengeInverseTransform(bytesToSeed(seedBytes));
    ASSERT_EQ(keyBytes(key), bytesFromHex("12345678"));
    ASSERT_EQ(buildChallengeKey(key), bytesFromHex("23274212345678"));
}
