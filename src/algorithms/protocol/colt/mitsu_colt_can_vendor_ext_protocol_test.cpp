#include <gtest/gtest.h>
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/testing/byte_test_utils.h"
using namespace mitsu_colt_can_vendor_ext;
using test_bytes::BytesFromHex;

TEST(TestMitsuColtCanVendorExtProtocol, challenge_transform_matches_known_vectors)
{
    // Vectors confirmed by exhaustive brute-force cross-check against
    // challengeInverseTransform() across the full 32-bit domain — 0
    // mismatches out of 4294967296. See design doc "Pre-verified facts".
    ASSERT_EQ(ChallengeTransform(0x00000000U), std::uint32_t(0xF2E207C5U));
    ASSERT_EQ(ChallengeTransform(0xFFFFFFFFU), std::uint32_t(0xE4C2C64DU));
    ASSERT_EQ(ChallengeTransform(0x12345678U), std::uint32_t(0x669E0CB4U));
    ASSERT_EQ(ChallengeTransform(0x00000001U), std::uint32_t(0xE0E207D2U));
    ASSERT_EQ(ChallengeTransform(0xDEADBEEFU), std::uint32_t(0xA5654127U));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_inverse_transform_matches_known_vectors)
{
    // Same five vectors as the forward test, inverted.
    ASSERT_EQ(ChallengeInverseTransform(0xF2E207C5U), std::uint32_t(0x00000000U));
    ASSERT_EQ(ChallengeInverseTransform(0xE4C2C64DU), std::uint32_t(0xFFFFFFFFU));
    ASSERT_EQ(ChallengeInverseTransform(0x669E0CB4U), std::uint32_t(0x12345678U));
    ASSERT_EQ(ChallengeInverseTransform(0xE0E207D2U), std::uint32_t(0x00000001U));
    ASSERT_EQ(ChallengeInverseTransform(0xA5654127U), std::uint32_t(0xDEADBEEFU));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_inverse_round_trips_with_forward)
{
    // Lightweight regression check standing in for the one-time
    // exhaustive 2^32 proof recorded in the design doc.
    static constexpr auto kValues = std::to_array<std::uint32_t>(
        {0x00000000U, 0xFFFFFFFFU, 0x12345678U, 0x00000001U, 0xDEADBEEFU, 0x80000000U, 0x7FFFFFFFU});
    for (std::uint32_t x : kValues)
    {
        ASSERT_EQ(ChallengeInverseTransform(ChallengeTransform(x)), x);
    }
}
TEST(TestMitsuColtCanVendorExtProtocol, byte_native_seed_and_key_helpers_round_trip)
{
    const bytes::Bytes seed_bytes = BytesFromHex("F2E207C5");
    ASSERT_EQ(BytesToSeed(seed_bytes), std::uint32_t(0xF2E207C5U));
    ASSERT_EQ(KeyBytes(0xF2E207C5U), seed_bytes);

    const std::uint32_t secret = 0x12345678U;
    const bytes::Bytes on_wire = KeyBytes(ChallengeTransform(secret));
    ASSERT_EQ(ChallengeInverseTransform(BytesToSeed(on_wire)), secret);
}
TEST(TestMitsuColtCanVendorExtProtocol, byte_native_challenge_frame_layout)
{
    ASSERT_EQ(BuildChallengeSeedRequest(), BytesFromHex("232741"));
    ASSERT_EQ(BuildChallengeKey(0x12345678U), BytesFromHex("23274212345678"));
}
TEST(TestMitsuColtCanVendorExtProtocol, challenge_key_frame_uses_inverse_key_bytes)
{
    const auto seed_bytes = BytesFromHex("669E0CB4");
    const std::uint32_t key = ChallengeInverseTransform(BytesToSeed(seed_bytes));
    ASSERT_EQ(KeyBytes(key), BytesFromHex("12345678"));
    ASSERT_EQ(BuildChallengeKey(key), BytesFromHex("23274212345678"));
}
