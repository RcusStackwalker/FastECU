#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/algorithms/protocol/testing/byte_test_utils.h"

using ::testing::ElementsAre;

TEST(SsmProtocolCorePortable, AddHeaderPrefixesTesterAndTarget)
{
    const bytes::Bytes payload{0xA0};
    const bytes::Bytes framed = SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10);
    ASSERT_GE(framed.size(), payload.size());
    EXPECT_EQ(framed[0], static_cast<bytes::Byte>(0x80));
}

TEST(SsmProtocolCore, AddHeaderBuildsTheFramedRequest)
{
    const bytes::Bytes payload{0xEF, 0x52};
    EXPECT_THAT(SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10),
                ElementsAre(0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3));
}

TEST(SsmProtocolCore, HasValidFrameAcceptsWhatAddHeaderProduces)
{
    const bytes::Bytes payload{0xEF, 0x52};
    const bytes::Bytes framed = SsmProtocol::addHeader(bytes::ByteView(payload), 0xF0, 0x10);
    EXPECT_TRUE(SsmProtocol::hasValidFrame(bytes::ByteView(framed), 0x10, 0xF0));
}

TEST(SsmProtocolCore, HasValidFrameRejectsACorruptedChecksum)
{
    bytes::Bytes framed{0x80, 0x10, 0xF0, 0x02, 0xEF, 0x52, 0xC3};
    framed.back() = 0xC4;
    EXPECT_FALSE(SsmProtocol::hasValidFrame(bytes::ByteView(framed), 0x10, 0xF0));
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
    EXPECT_EQ(SsmProtocol::kIndexTransformationStock, kExpected);
}

TEST(SsmProtocolCore, EcutekIndexTransformationMatchesTheLegacyTable)
{
    constexpr std::array<std::uint8_t, 32> kExpected{0x4, 0x2, 0x5, 0x1, 0x8, 0xC, 0xD, 0x8, 0xA, 0xD, 0x2,
                                                     0xB, 0xF, 0x4, 0x0, 0x3, 0xB, 0x4, 0x6, 0x0, 0xF, 0x2,
                                                     0xD, 0x9, 0x5, 0xC, 0x1, 0xA, 0x3, 0xD, 0xE, 0x8};
    EXPECT_EQ(SsmProtocol::kIndexTransformationEcutek, kExpected);
}

// The whole reason these two are named rather than spelled out at each call
// site: they differ in exactly their first five entries and are identical in
// the remaining twenty-seven. Two inline 32-entry literals do not show that;
// this does, and it fails if either table drifts toward the other.
TEST(SsmProtocolCore, StockAndEcutekIndexTransformationsDifferOnlyInTheFirstFiveEntries)
{
    EXPECT_NE(SsmProtocol::kIndexTransformationStock, SsmProtocol::kIndexTransformationEcutek);
    EXPECT_TRUE(std::equal(SsmProtocol::kIndexTransformationStock.begin() + 5,
                           SsmProtocol::kIndexTransformationStock.end(),
                           SsmProtocol::kIndexTransformationEcutek.begin() + 5));
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
    EXPECT_THAT(SsmProtocol::calculateSeedKey(bytes::ByteView(test_bytes::bytesFromHex("12345678")), kCommonSeedTable,
                                              SsmProtocol::kIndexTransformationStock),
                test_bytes::BytesEq(test_bytes::bytesFromHex("2daa46dc")));
}

TEST(SsmProtocolCore, CalculateSeedKeyMatchesTheAlternateTableVector)
{
    EXPECT_THAT(SsmProtocol::calculateSeedKey(bytes::ByteView(test_bytes::bytesFromHex("89abcdef")),
                                              kAlternateSeedTable, kAlternateTransformTable),
                test_bytes::BytesEq(test_bytes::bytesFromHex("408d111d")));
}

TEST(SsmProtocolCore, CalculatePayloadMatchesTheCommonDensoVector)
{
    EXPECT_THAT(SsmProtocol::calculatePayload(bytes::ByteView(test_bytes::bytesFromHex("0011223344556677")), 8,
                                              kPayloadTable, SsmProtocol::kIndexTransformationStock),
                test_bytes::BytesEq(test_bytes::bytesFromHex("ed9fd931afacd594")));
}

TEST(SsmProtocolCore, CalculatePayloadTruncatesToAFourByteBoundary)
{
    EXPECT_THAT(SsmProtocol::calculatePayload(bytes::ByteView(test_bytes::bytesFromHex("0011223344")), 5, kPayloadTable,
                                              SsmProtocol::kIndexTransformationStock),
                test_bytes::BytesEq(test_bytes::bytesFromHex("ed9fd931")));
}

TEST(SsmProtocolCore, HasPayloadPrefixAcceptsAMatchingPrefix)
{
    const bytes::Bytes response =
        SsmProtocol::addHeader(bytes::ByteView(test_bytes::bytesFromHex("EF5201")), 0xF0, 0x10);

    EXPECT_TRUE(SsmProtocol::hasPayloadPrefix(bytes::ByteView(response),
                                              bytes::ByteView(test_bytes::bytesFromHex("EF52")), 0x10, 0xF0));
}

TEST(SsmProtocolCore, HasPayloadPrefixRejectsAMismatchedPrefixATooLongPrefixOrABadChecksum)
{
    const bytes::Bytes response =
        SsmProtocol::addHeader(bytes::ByteView(test_bytes::bytesFromHex("EF5201")), 0xF0, 0x10);
    bytes::Bytes badChecksum = response;
    badChecksum.back() = 0x00;

    EXPECT_FALSE(SsmProtocol::hasPayloadPrefix(bytes::ByteView(response),
                                               bytes::ByteView(test_bytes::bytesFromHex("EF53")), 0x10, 0xF0));
    EXPECT_FALSE(SsmProtocol::hasPayloadPrefix(bytes::ByteView(response),
                                               bytes::ByteView(test_bytes::bytesFromHex("EF520100")), 0x10, 0xF0));
    EXPECT_FALSE(SsmProtocol::hasPayloadPrefix(bytes::ByteView(badChecksum),
                                               bytes::ByteView(test_bytes::bytesFromHex("EF52")), 0x10, 0xF0));
}

TEST(SsmProtocolCore, HasValidFrameRejectsAShortFrameOrAWrongReceiverSenderOrLength)
{
    const bytes::Bytes response = SsmProtocol::addHeader(bytes::ByteView(test_bytes::bytesFromHex("EF52")), 0xF0, 0x10);
    bytes::Bytes badLength = response;
    badLength[3] = 0x03;

    EXPECT_FALSE(SsmProtocol::hasValidFrame(bytes::ByteView(test_bytes::bytesFromHex("8010F0")), 0x10, 0xF0));
    EXPECT_FALSE(SsmProtocol::hasValidFrame(bytes::ByteView(response), 0x11, 0xF0));
    EXPECT_FALSE(SsmProtocol::hasValidFrame(bytes::ByteView(response), 0x10, 0xF1));
    EXPECT_FALSE(SsmProtocol::hasValidFrame(bytes::ByteView(badLength), 0x10, 0xF0));
}
