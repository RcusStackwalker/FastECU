#include "src/backend/diagnostics/obd_frames.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace fastecu::diagnostics;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Optional;

namespace
{
bytes::Bytes B(std::initializer_list<int> values)
{
    bytes::Bytes out;
    for (int v : values)
    {
        out.push_back(static_cast<bytes::Byte>(v));
    }
    return out;
}
} // namespace

TEST(ObdFrames, RequestsCarryTheCanIdPrefixOnlyOnIso15765)
{
    EXPECT_THAT(BuildRequest(ObdProtocol::kIso9141, 0x7E0, B({0x01, 0x00})), ElementsAre(0x01, 0x00));
    EXPECT_THAT(BuildRequest(ObdProtocol::kIso15765, 0x7E0, B({0x03})), ElementsAre(0x00, 0x00, 0x07, 0xE0, 0x03));
}

TEST(ObdFrames, CheckResponse)
{
    // K-Line: response byte at index 3.
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso9141, B({0x48, 0x6B, 0x10}), 0x01, 0x00), ResponseCheck::kShort);
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso9141, B({0x48, 0x6B, 0x10, 0x41, 0x00, 0xAA}), 0x01, 0x00),
              ResponseCheck::kOk);
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso9141, B({0x48, 0x6B, 0x10, 0x7F, 0x01, 0x12}), 0x01, 0x00),
              ResponseCheck::kNrc);
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso9141, B({0x48, 0x6B, 0x10, 0x42, 0x00}), 0x01, 0x00),
              ResponseCheck::kWrongId);
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso9141, B({0x48, 0x6B, 0x10, 0x41, 0x20}), 0x01, 0x00),
              ResponseCheck::kWrongId);
    // A PID echo byte missing entirely is a wrong response, not an out-of-range read.
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso9141, B({0x48, 0x6B, 0x10, 0x41}), 0x01, 0x00), ResponseCheck::kWrongId);
    // No PID to echo (DTC list requests).
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso9141, B({0x48, 0x6B, 0x10, 0x43}), 0x03, std::nullopt),
              ResponseCheck::kOk);
    // PIDs >= 0x80 compare unsigned (spec behavior change 5).
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso9141, B({0x48, 0x6B, 0x10, 0x41, 0xA0, 0x00}), 0x01, 0xA0),
              ResponseCheck::kOk);
    // iso15765: index 4.
    EXPECT_EQ(CheckResponse(ObdProtocol::kIso15765, B({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00}), 0x01, 0x00),
              ResponseCheck::kOk);
}

TEST(ObdFrames, KlineDataUnframingHeuristics)
{
    // Checksum dropped first; then by the remaining length m:
    EXPECT_THAT(UnframeDataResponse(ObdProtocol::kIso9141, B({0x11})), IsEmpty()); // m == 0
    EXPECT_THAT(UnframeDataResponse(ObdProtocol::kIso9141, B({1, 2, 3, 4, 5, 6, 0xCC})),
                ElementsAre(6)); // m < 7: last byte
    EXPECT_THAT(UnframeDataResponse(ObdProtocol::kIso9141, B({1, 2, 3, 4, 5, 6, 7, 8, 9, 0xCC})),
                ElementsAre(6, 7, 8, 9)); // m < 10: drop 5
    EXPECT_THAT(UnframeDataResponse(ObdProtocol::kIso9141, B({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xCC})),
                ElementsAre(7, 8, 9, 10)); // else: drop 6
}

TEST(ObdFrames, KlineDtcListUnframingHeuristics)
{
    EXPECT_THAT(UnframeDtcListResponse(ObdProtocol::kIso9141, B({1, 2, 3, 4, 5, 6, 0xCC})), ElementsAre(6));
    EXPECT_THAT(UnframeDtcListResponse(ObdProtocol::kIso9141, B({1, 2, 3, 4, 5, 6, 7, 0xCC})), ElementsAre(5, 6, 7));
}

TEST(ObdFrames, Iso15765Unframing)
{
    const auto frame = B({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00, 0xBE, 0x1F});
    EXPECT_THAT(UnframeDataResponse(ObdProtocol::kIso15765, frame), ElementsAre(0x1F));
    EXPECT_THAT(UnframeDtcListResponse(ObdProtocol::kIso15765, B({0x00, 0x00, 0x07, 0xE8, 0x43, 0x01, 0x01, 0x33})),
                ElementsAre(0x01, 0x33));
    EXPECT_THAT(UnframeDataResponse(ObdProtocol::kIso15765, B({0x00, 0x00, 0x07})), IsEmpty());
}

TEST(ObdFrames, FiveBaudHeaderOnOpenPortComparesAsciiDigits)
{
    // Pinned as-is: the J2534 branch compares bytes to ASCII '8' and 'f'.
    const auto iso9141 = B({0, 0, 0, 0, 0, '8', 0, '8'});
    EXPECT_THAT(FiveBaudHeader(ObdProtocol::kIso9141, iso9141, true), Optional(KlineHeader::kIso9141));
    EXPECT_EQ(FiveBaudHeader(ObdProtocol::kIso14230, iso9141, true), std::nullopt);
    const auto iso14230 = B({0, 0, 0, 0, 0, 0, 0, 0, '8', 'f'});
    EXPECT_THAT(FiveBaudHeader(ObdProtocol::kIso14230, iso14230, true), Optional(KlineHeader::kIso14230));
    // Short responses are rejected, not read out of range.
    EXPECT_EQ(FiveBaudHeader(ObdProtocol::kIso9141, B({0, 0, 0, 0, 0, '8', 0}), true), std::nullopt);
    EXPECT_EQ(FiveBaudHeader(ObdProtocol::kIso14230, B({0, 0, 0, 0, 0, 0, 0, 0, '8'}), true), std::nullopt);
}

TEST(ObdFrames, FiveBaudHeaderOnDirectSerialIgnoresTheRequestedProtocol)
{
    EXPECT_THAT(FiveBaudHeader(ObdProtocol::kIso14230, B({0x55, 0x08, 0x08}), false), Optional(KlineHeader::kIso9141));
    EXPECT_THAT(FiveBaudHeader(ObdProtocol::kIso9141, B({0x55, 0xEF, 0x8F}), false), Optional(KlineHeader::kIso14230));
    EXPECT_EQ(FiveBaudHeader(ObdProtocol::kIso9141, B({0x55, 0x00, 0x00}), false), std::nullopt);
    EXPECT_EQ(FiveBaudHeader(ObdProtocol::kIso9141, B({0x55, 0x08}), false), std::nullopt);
    EXPECT_EQ(FiveBaudHeader(ObdProtocol::kIso9141, bytes::Bytes{}, false), std::nullopt);
}

TEST(ObdFrames, FastInitAcceptance)
{
    EXPECT_TRUE(FastInitAccepted(B({0x83, 0xF1, 0x10, 0xC1, 0xE9, 0x8F, 0xAE})));
    EXPECT_FALSE(FastInitAccepted(B({0x83, 0xF1, 0x10, 0xC1, 0xE9})));
    EXPECT_FALSE(FastInitAccepted(B({0x83, 0xF1, 0x11, 0xC1, 0xE9, 0x8F})));
}

TEST(ObdFrames, Formatting)
{
    EXPECT_EQ(FormatHex(B({0x83, 0x0A})), "83 0a ");
    EXPECT_EQ(FormatPidPageLabel(1, B({0xBE})), "Supported PIDs 0x21-0x40: be ");
    EXPECT_EQ(FormatSupportedPids(0, B({0x80})), "0x01 0x00 0x00 0x00 0x00 0x00 0x00 0x00 ");
    EXPECT_EQ(FormatSupportedPids(0, B({0x01})), "0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x08 ");
}

TEST(ObdFrames, DtcDecodingDropsZerosSortsAndIgnoresAnOddTail)
{
    EXPECT_THAT(DecodeDtcs(B({0x04, 0x20, 0x00, 0x00, 0x01, 0x33, 0x7F})), ElementsAre(0x0133, 0x0420));
}
