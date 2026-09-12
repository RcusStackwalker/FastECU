#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

#include <algorithm>
#include <array>
#include <cstdint>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

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
