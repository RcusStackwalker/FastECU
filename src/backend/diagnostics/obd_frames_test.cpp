#include "src/backend/diagnostics/obd_frames.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace fastecu::diagnostics;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Optional;

namespace
{
bytes::Bytes b(std::initializer_list<int> values)
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
    EXPECT_THAT(build_request(ObdProtocol::Iso9141, 0x7E0, b({0x01, 0x00})), ElementsAre(0x01, 0x00));
    EXPECT_THAT(build_request(ObdProtocol::Iso15765, 0x7E0, b({0x03})), ElementsAre(0x00, 0x00, 0x07, 0xE0, 0x03));
}

TEST(ObdFrames, CheckResponse)
{
    // K-Line: response byte at index 3.
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10}), 0x01, 0x00), ResponseCheck::Short);
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x41, 0x00, 0xAA}), 0x01, 0x00),
              ResponseCheck::Ok);
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x7F, 0x01, 0x12}), 0x01, 0x00),
              ResponseCheck::Nrc);
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x42, 0x00}), 0x01, 0x00),
              ResponseCheck::WrongId);
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x41, 0x20}), 0x01, 0x00),
              ResponseCheck::WrongId);
    // A PID echo byte missing entirely is a wrong response, not an out-of-range read.
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x41}), 0x01, 0x00), ResponseCheck::WrongId);
    // No PID to echo (DTC list requests).
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x43}), 0x03, std::nullopt), ResponseCheck::Ok);
    // PIDs >= 0x80 compare unsigned (spec behavior change 5).
    EXPECT_EQ(check_response(ObdProtocol::Iso9141, b({0x48, 0x6B, 0x10, 0x41, 0xA0, 0x00}), 0x01, 0xA0),
              ResponseCheck::Ok);
    // iso15765: index 4.
    EXPECT_EQ(check_response(ObdProtocol::Iso15765, b({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00}), 0x01, 0x00),
              ResponseCheck::Ok);
}

TEST(ObdFrames, KlineDataUnframingHeuristics)
{
    // Checksum dropped first; then by the remaining length m:
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso9141, b({0x11})), IsEmpty()); // m == 0
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 0xCC})),
                ElementsAre(6)); // m < 7: last byte
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 7, 8, 9, 0xCC})),
                ElementsAre(6, 7, 8, 9)); // m < 10: drop 5
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0xCC})),
                ElementsAre(7, 8, 9, 10)); // else: drop 6
}

TEST(ObdFrames, KlineDtcListUnframingHeuristics)
{
    EXPECT_THAT(unframe_dtc_list_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 0xCC})), ElementsAre(6));
    EXPECT_THAT(unframe_dtc_list_response(ObdProtocol::Iso9141, b({1, 2, 3, 4, 5, 6, 7, 0xCC})), ElementsAre(5, 6, 7));
}

TEST(ObdFrames, Iso15765Unframing)
{
    const auto frame = b({0x00, 0x00, 0x07, 0xE8, 0x41, 0x00, 0xBE, 0x1F});
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso15765, frame), ElementsAre(0x1F));
    EXPECT_THAT(unframe_dtc_list_response(ObdProtocol::Iso15765, b({0x00, 0x00, 0x07, 0xE8, 0x43, 0x01, 0x01, 0x33})),
                ElementsAre(0x01, 0x33));
    EXPECT_THAT(unframe_data_response(ObdProtocol::Iso15765, b({0x00, 0x00, 0x07})), IsEmpty());
}

TEST(ObdFrames, FiveBaudHeaderOnOpenPortComparesAsciiDigits)
{
    // Pinned as-is: the J2534 branch compares bytes to ASCII '8' and 'f'.
    const auto iso9141 = b({0, 0, 0, 0, 0, '8', 0, '8'});
    EXPECT_THAT(five_baud_header(ObdProtocol::Iso9141, iso9141, true), Optional(KlineHeader::Iso9141));
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso14230, iso9141, true), std::nullopt);
    const auto iso14230 = b({0, 0, 0, 0, 0, 0, 0, 0, '8', 'f'});
    EXPECT_THAT(five_baud_header(ObdProtocol::Iso14230, iso14230, true), Optional(KlineHeader::Iso14230));
    // Short responses are rejected, not read out of range.
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso9141, b({0, 0, 0, 0, 0, '8', 0}), true), std::nullopt);
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso14230, b({0, 0, 0, 0, 0, 0, 0, 0, '8'}), true), std::nullopt);
}

TEST(ObdFrames, FiveBaudHeaderOnDirectSerialIgnoresTheRequestedProtocol)
{
    EXPECT_THAT(five_baud_header(ObdProtocol::Iso14230, b({0x55, 0x08, 0x08}), false), Optional(KlineHeader::Iso9141));
    EXPECT_THAT(five_baud_header(ObdProtocol::Iso9141, b({0x55, 0xEF, 0x8F}), false), Optional(KlineHeader::Iso14230));
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso9141, b({0x55, 0x00, 0x00}), false), std::nullopt);
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso9141, b({0x55, 0x08}), false), std::nullopt);
    EXPECT_EQ(five_baud_header(ObdProtocol::Iso9141, bytes::Bytes{}, false), std::nullopt);
}

TEST(ObdFrames, FastInitAcceptance)
{
    EXPECT_TRUE(fast_init_accepted(b({0x83, 0xF1, 0x10, 0xC1, 0xE9, 0x8F, 0xAE})));
    EXPECT_FALSE(fast_init_accepted(b({0x83, 0xF1, 0x10, 0xC1, 0xE9})));
    EXPECT_FALSE(fast_init_accepted(b({0x83, 0xF1, 0x11, 0xC1, 0xE9, 0x8F})));
}

TEST(ObdFrames, Formatting)
{
    EXPECT_EQ(format_hex(b({0x83, 0x0A})), "83 0a ");
    EXPECT_EQ(format_pid_page_label(1, b({0xBE})), "Supported PIDs 0x21-0x40: be ");
    EXPECT_EQ(format_supported_pids(0, b({0x80})), "0x01 0x00 0x00 0x00 0x00 0x00 0x00 0x00 ");
    EXPECT_EQ(format_supported_pids(0, b({0x01})), "0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x08 ");
}

TEST(ObdFrames, DtcDecodingDropsZerosSortsAndIgnoresAnOddTail)
{
    EXPECT_THAT(decode_dtcs(b({0x04, 0x20, 0x00, 0x00, 0x01, 0x33, 0x7F})), ElementsAre(0x0133, 0x0420));
}
