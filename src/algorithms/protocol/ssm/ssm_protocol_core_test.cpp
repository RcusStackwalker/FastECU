#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <random>
#include <span>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/algorithms/protocol/testing/byte_test_utils.h"

using ::testing::ElementsAre;

TEST(SsmProtocolCorePortable, AddHeaderPrefixesTesterAndTarget)
{
    const bytes::Bytes payload{0xA0};
    const bytes::Bytes framed = ssm_protocol::AddHeader(bytes::ByteView(payload), 0xF0, 0x10);
    ASSERT_GE(framed.size(), payload.size());
    EXPECT_EQ(framed[0], static_cast<bytes::Byte>(0x80));
}

TEST(SsmProtocolCore, AddHeaderBuildsTheFramedRequest)
{
    const bytes::Bytes payload{0xEF, 0x52};
    EXPECT_THAT(ssm_protocol::AddHeader(bytes::ByteView(payload), 0xF0, 0x10),
                ElementsAre(0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3));
}

TEST(SsmProtocolCore, HasValidFrameAcceptsWhatAddHeaderProduces)
{
    const bytes::Bytes payload{0xEF, 0x52};
    const bytes::Bytes framed = ssm_protocol::AddHeader(bytes::ByteView(payload), 0xF0, 0x10);
    EXPECT_TRUE(ssm_protocol::HasValidFrame(bytes::ByteView(framed), 0x10, 0xF0));
}

TEST(SsmProtocolCore, HasValidFrameRejectsACorruptedChecksum)
{
    bytes::Bytes framed{0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3};
    framed.back() = 0xC4;
    EXPECT_FALSE(ssm_protocol::HasValidFrame(bytes::ByteView(framed), 0x10, 0xF0));
}

// The two index transformations calculateSeedKey and calculatePayload take.
//
// Transcribed a second time here, independently of the header, so a slipped
// digit there fails rather than passing silently -- the same discipline
// src/backend/flash/ecu/denso_iso15765_can_common_test.cpp applies to its
// per-cluster tables.
TEST(SsmProtocolCore, StockIndexTransformationMatchesTheLegacyTable)
{
    constexpr std::array<std::uint8_t, 32> kExpected{0x5, 0x6, 0x7, 0x1, 0x9, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                     0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                     0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    EXPECT_EQ(ssm_protocol::kIndexTransformationStock, kExpected);
}

TEST(SsmProtocolCore, EcutekIndexTransformationMatchesTheLegacyTable)
{
    constexpr std::array<std::uint8_t, 32> kExpected{0x4, 0x2, 0x5, 0x1, 0x8, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                     0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                     0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    EXPECT_EQ(ssm_protocol::kIndexTransformationEcutek, kExpected);
}

// The whole reason these two are named rather than spelled out at each call
// site: they differ in exactly their first five entries and are identical in
// the remaining twenty-seven. Two inline 32-entry literals do not show that;
// this does, and it fails if either table drifts toward the other.
TEST(SsmProtocolCore, StockAndEcutekIndexTransformationsDifferOnlyInTheFirstFiveEntries)
{
    EXPECT_NE(ssm_protocol::kIndexTransformationStock, ssm_protocol::kIndexTransformationEcutek);
    EXPECT_TRUE(std::equal(ssm_protocol::kIndexTransformationStock.begin() + 5,
                           ssm_protocol::kIndexTransformationStock.end(),
                           ssm_protocol::kIndexTransformationEcutek.begin() + 5));
}

// Ported from the deleted src/algorithms/protocol/ssm/qt_compat/ssm_protocol_qt_compat_test.cpp
// so calculateSeedKey and calculatePayload's known-answer vectors survive the
// Qt shim's removal. kAlternateSeedTable/kAlternateTransformTable are
// synthetic fixtures proving the transform generalizes beyond
// kIndexTransformationStock; they are not production tables.
namespace
{
constexpr auto kCommonSeedTable =
    std::to_array<std::uint16_t>({0x90A1, 0x2F92, 0xDE3C, 0xCDC0, 0x1A99, 0x437C, 0xF91B, 0xDB57, 0x96BA, 0xDE10,
                                  0xFCAF, 0x3F31, 0xF47F, 0x0BB6, 0x16E9, 0x4645});

constexpr auto kAlternateSeedTable =
    std::to_array<std::uint16_t>({0x8765, 0x2345, 0xA5A5, 0x1357, 0x2468, 0xACE0, 0x0ACE, 0x55AA, 0xAA55, 0x1020,
                                  0x3040, 0x5060, 0x7080, 0x90A0, 0xB0C0, 0xD0E0});

constexpr auto kAlternateTransformTable =
    std::to_array<std::uint8_t>({0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF,
                                 0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0x7, 0x6, 0x5, 0x4, 0x3, 0x2, 0x1, 0x0});

constexpr auto kPayloadTable = std::to_array<std::uint16_t>({0xC85B, 0x32C0, 0xE282, 0x92A0});
} // namespace

TEST(SsmProtocolCore, CalculateSeedKeyMatchesTheCommonDensoVector)
{
    EXPECT_THAT(ssm_protocol::CalculateSeedKey(bytes::ByteView(test_bytes::BytesFromHex("12345678")), kCommonSeedTable,
                                               ssm_protocol::kIndexTransformationStock),
                test_bytes::BytesEq(test_bytes::BytesFromHex("2daa46dc")));
}

TEST(SsmProtocolCore, CalculateSeedKeyMatchesTheAlternateTableVector)
{
    EXPECT_THAT(ssm_protocol::CalculateSeedKey(bytes::ByteView(test_bytes::BytesFromHex("89abcdef")),
                                               kAlternateSeedTable, kAlternateTransformTable),
                test_bytes::BytesEq(test_bytes::BytesFromHex("408d111d")));
}

TEST(SsmProtocolCore, CalculatePayloadMatchesTheCommonDensoVector)
{
    EXPECT_THAT(ssm_protocol::CalculatePayload(bytes::ByteView(test_bytes::BytesFromHex("0011223344556677")),
                                               kPayloadTable, ssm_protocol::kIndexTransformationStock),
                test_bytes::BytesEq(test_bytes::BytesFromHex("ed9fd931afacd594")));
}

TEST(SsmProtocolCore, CalculatePayloadTruncatesToAFourByteBoundary)
{
    EXPECT_THAT(ssm_protocol::CalculatePayload(bytes::ByteView(test_bytes::BytesFromHex("0011223344")), kPayloadTable,
                                               ssm_protocol::kIndexTransformationStock),
                test_bytes::BytesEq(test_bytes::BytesFromHex("ed9fd931")));
}

TEST(SsmProtocolCore, HasPayloadPrefixAcceptsAMatchingPrefix)
{
    const bytes::Bytes response =
        ssm_protocol::AddHeader(bytes::ByteView(test_bytes::BytesFromHex("EF5201")), 0xF0, 0x10);

    EXPECT_TRUE(ssm_protocol::HasPayloadPrefix(bytes::ByteView(response),
                                               bytes::ByteView(test_bytes::BytesFromHex("EF52")), 0x10, 0xF0));
}

TEST(SsmProtocolCore, HasPayloadPrefixRejectsAMismatchedPrefixATooLongPrefixOrABadChecksum)
{
    const bytes::Bytes response =
        ssm_protocol::AddHeader(bytes::ByteView(test_bytes::BytesFromHex("EF5201")), 0xF0, 0x10);
    bytes::Bytes bad_checksum = response;
    bad_checksum.back() = 0x00;

    EXPECT_FALSE(ssm_protocol::HasPayloadPrefix(bytes::ByteView(response),
                                                bytes::ByteView(test_bytes::BytesFromHex("EF53")), 0x10, 0xF0));
    EXPECT_FALSE(ssm_protocol::HasPayloadPrefix(bytes::ByteView(response),
                                                bytes::ByteView(test_bytes::BytesFromHex("EF520100")), 0x10, 0xF0));
    EXPECT_FALSE(ssm_protocol::HasPayloadPrefix(bytes::ByteView(bad_checksum),
                                                bytes::ByteView(test_bytes::BytesFromHex("EF52")), 0x10, 0xF0));
}

TEST(SsmProtocolCore, HasValidFrameRejectsAShortFrameOrAWrongReceiverSenderOrLength)
{
    const bytes::Bytes response =
        ssm_protocol::AddHeader(bytes::ByteView(test_bytes::BytesFromHex("EF52")), 0xF0, 0x10);
    bytes::Bytes bad_length = response;
    bad_length[3] = 0x03;

    EXPECT_FALSE(ssm_protocol::HasValidFrame(bytes::ByteView(test_bytes::BytesFromHex("8010F0")), 0x10, 0xF0));
    EXPECT_FALSE(ssm_protocol::HasValidFrame(bytes::ByteView(response), 0x11, 0xF0));
    EXPECT_FALSE(ssm_protocol::HasValidFrame(bytes::ByteView(response), 0x10, 0xF1));
    EXPECT_FALSE(ssm_protocol::HasValidFrame(bytes::ByteView(bad_length), 0x10, 0xF0));
}

namespace
{
// Verbatim copy of the original per-word implementation, kept as an oracle.
template <bool Reverse, std::size_t Rounds>
std::uint32_t ReferenceTransformWord(std::uint32_t word, std::span<const std::uint16_t, Rounds> keytogenerateindex,
                                     std::span<const std::uint8_t, 32> indextransformation)
{
    for (std::size_t r = 0; r < Rounds; ++r)
    {
        const std::size_t ki = Reverse ? (Rounds - 1 - r) : r;
        const std::uint16_t wordtogenerateindex = word;
        const std::uint16_t wordtobeencrypted = word >> 16U;
        std::uint32_t index = wordtogenerateindex ^ keytogenerateindex[ki];
        index += index << 16U;

        std::uint16_t encryptionkey = static_cast<std::uint16_t>(
            indextransformation[index & 0x1FU] + (indextransformation[(index >> 4U) & 0x1FU] << 4U) +
            (indextransformation[(index >> 8U) & 0x1FU] << 8U) + (indextransformation[(index >> 12U) & 0x1FU] << 12U));

        encryptionkey = (encryptionkey >> 3U) + (encryptionkey << 13U);
        word = (encryptionkey ^ wordtobeencrypted) + (wordtogenerateindex << 16U);
    }
    return std::rotr(word, 16);
}

bytes::Bytes BigEndian(std::uint32_t word)
{
    return {bytes::Byte(word >> 24U), bytes::Byte(word >> 16U), bytes::Byte(word >> 8U), bytes::Byte(word)};
}
} // namespace

TEST(SsmProtocolCore, SeedKeyAndPayloadMatchTheReferenceTransformOnRandomInputs)
{
    std::mt19937 rng(12345);
    for (int round = 0; round < 200; ++round)
    {
        std::array<std::uint8_t, 32> table{};
        std::array<std::uint16_t, 16> seed_keys{};
        for (auto& entry : table)
        {
            entry = static_cast<std::uint8_t>(rng()); // entries above 15 exercise carries between nibbles
        }
        for (auto& key : seed_keys)
        {
            key = static_cast<std::uint16_t>(rng());
        }
        const std::span<const std::uint16_t, 4> payload_keys(seed_keys.data(), 4);

        for (int i = 0; i < 200; ++i)
        {
            const auto word = static_cast<std::uint32_t>(rng());
            EXPECT_THAT(
                ssm_protocol::CalculateSeedKey(bytes::ByteView(BigEndian(word)), seed_keys, table),
                test_bytes::BytesEq(BigEndian(ReferenceTransformWord<true, 16>(word, std::span(seed_keys), table))));
            EXPECT_THAT(ssm_protocol::CalculatePayload(bytes::ByteView(BigEndian(word)), payload_keys, table),
                        test_bytes::BytesEq(BigEndian(ReferenceTransformWord<false, 4>(word, payload_keys, table))));
        }
    }
}

namespace
{

// The original per-word transform, kept as an oracle for CalculatePayload's
// paired-table fast path.
std::uint32_t ReferencePayloadWord(std::uint32_t word, const std::array<std::uint16_t, 4>& keys,
                                   const std::array<std::uint8_t, 32>& table)
{
    for (std::size_t r = 0; r < keys.size(); ++r)
    {
        const std::uint16_t low = static_cast<std::uint16_t>(word);
        const std::uint16_t high = static_cast<std::uint16_t>(word >> 16U);
        std::uint32_t index = low ^ keys[r];
        index += index << 16U;
        std::uint16_t key =
            static_cast<std::uint16_t>(table[index & 0x1FU] + (table[(index >> 4U) & 0x1FU] << 4U) +
                                       (table[(index >> 8U) & 0x1FU] << 8U) + (table[(index >> 12U) & 0x1FU] << 12U));
        key = static_cast<std::uint16_t>((key >> 3U) + (key << 13U));
        word = (static_cast<std::uint32_t>(key ^ high)) + (static_cast<std::uint32_t>(low) << 16U);
    }
    return (word >> 16U) | (word << 16U);
}

} // namespace

TEST(SsmProtocolCore, CalculatePayloadMatchesTheReferenceTransformForAnyTableAndLength)
{
    std::uint32_t state = 12345U;
    const auto next = [&state]
    {
        state = state * 1664525U + 1013904223U;
        return state >> 8U;
    };
    for (const std::size_t words : {0U, 1U, 2U, 63U, 64U, 65U, 200U})
    {
        for (int trial = 0; trial < 20; ++trial)
        {
            std::array<std::uint8_t, 32> table{};
            for (auto& entry : table)
            {
                entry = static_cast<std::uint8_t>(next()); // entries above 0xF included on purpose
            }
            std::array<std::uint16_t, 4> keys{};
            for (auto& key : keys)
            {
                key = static_cast<std::uint16_t>(next());
            }
            bytes::Bytes input(words * 4 + 3);
            for (auto& byte : input)
            {
                byte = static_cast<bytes::Byte>(next());
            }
            bytes::Bytes expected;
            for (std::size_t w = 0; w < words; ++w)
            {
                const std::uint32_t in = (std::uint32_t{input[w * 4]} << 24U) |
                                         (std::uint32_t{input[w * 4 + 1]} << 16U) |
                                         (std::uint32_t{input[w * 4 + 2]} << 8U) | input[w * 4 + 3];
                const std::uint32_t out = ReferencePayloadWord(in, keys, table);
                for (int shift = 24; shift >= 0; shift -= 8)
                {
                    expected.push_back(static_cast<bytes::Byte>(out >> shift));
                }
            }
            EXPECT_EQ(ssm_protocol::CalculatePayload(bytes::ByteView(input), keys, table), expected)
                << "words=" << words << " trial=" << trial;
        }
    }
}
