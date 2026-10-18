#include "src/backend/flash/testing/wire_transcript.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <numeric>
#include <string>

namespace fastecu::flash
{
namespace
{

using Kind = WireTranscriptStep::Kind;
using ::testing::Eq;
using ::testing::HasSubstr;

bytes::Bytes Ramp256()
{
    bytes::Bytes image(256);
    std::iota(image.begin(), image.end(), bytes::Byte{0});
    return image;
}

TEST(WireTranscriptTest, ParsesEachStepKind)
{
    const auto parsed = ParseWireTranscript("# section connect\n"
                                            "> 00 00 07 e0 10 85\n"
                                            "@ read-timeout 1000\n"
                                            "< 00 00 07 e8 50 85\n"
                                            "< none\n"
                                            "@ sleep 20\n",
                                            {});
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    const WireTranscript expected = {
        {.kind = Kind::kSection, .label = "connect"},
        {.kind = Kind::kWrite, .frame = {0x00, 0x00, 0x07, 0xe0, 0x10, 0x85}},
        {.kind = Kind::kReadTimeout, .duration = std::chrono::milliseconds(1000)},
        {.kind = Kind::kRead, .frame = {0x00, 0x00, 0x07, 0xe8, 0x50, 0x85}},
        {.kind = Kind::kNoFrame},
        {.kind = Kind::kSleep, .duration = std::chrono::milliseconds(20)},
    };
    EXPECT_THAT(*parsed, Eq(expected));
}

TEST(WireTranscriptTest, IgnoresBlankLinesAndComments)
{
    const auto parsed = ParseWireTranscript("\n## note\n> 01\n\n", {});
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_THAT(*parsed, Eq(WireTranscript{{.kind = Kind::kWrite, .frame = {0x01}}}));
}

TEST(WireTranscriptTest, ExpandsImageAndFillReferences)
{
    const auto image = Ramp256();
    const auto parsed = ParseWireTranscript("< 63 {image 0x10+4} {fill ee 3}", image);
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    ASSERT_EQ(parsed->size(), 1U);
    EXPECT_THAT((*parsed)[0].frame, Eq(bytes::Bytes{0x63, 0x10, 0x11, 0x12, 0x13, 0xee, 0xee, 0xee}));
}

TEST(WireTranscriptTest, RejectsAReferencePastTheImage)
{
    const auto image = Ramp256();
    EXPECT_THAT(ParseWireTranscript("> {image 0xfe+4}", image), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(WireTranscriptTest, RejectsAnUnknownLine)
{
    const auto parsed = ParseWireTranscript("? 00", {});
    ASSERT_THAT(parsed, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(parsed.error().detail, HasSubstr("line 1"));
}

TEST(WireTranscriptTest, RejectsMalformedHex)
{
    EXPECT_THAT(ParseWireTranscript("> 0g", {}), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(ParseWireTranscript("> 123", {}), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(ParseWireTranscript("@ sleep x", {}), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(WireTranscriptTest, FormatRoundTrips)
{
    const auto image = Ramp256();
    bytes::Bytes write = {0x36, 0x01, 0x00, 0x00, 0x40};
    write.insert(write.end(), image.begin() + 0x40, image.begin() + 0x80);
    const WireTranscript transcript = {
        {.kind = Kind::kSection, .label = "data"},
        {.kind = Kind::kWrite, .frame = write},
        {.kind = Kind::kRead, .frame = bytes::Bytes(40, 0xff)},
        {.kind = Kind::kNoFrame},
        {.kind = Kind::kReadTimeout, .duration = std::chrono::milliseconds(5)},
        {.kind = Kind::kSleep, .duration = std::chrono::milliseconds(7)},
    };
    const std::string text = FormatWireTranscript(transcript, image);
    EXPECT_THAT(text, HasSubstr("{image 0x40+64}"));
    EXPECT_THAT(text, HasSubstr("{fill ff 40}"));
    const auto parsed = ParseWireTranscript(text, image);
    ASSERT_THAT(parsed, fastecu::testing::IsOk());
    EXPECT_THAT(*parsed, Eq(transcript));
}

TEST(WireTranscriptTest, SequentialChunksReferenceSequentialOffsets)
{
    bytes::Bytes image(0x100, 0);
    for (std::size_t i = 0; i < image.size(); ++i)
    {
        image[i] = static_cast<bytes::Byte>(i % 32U); // repeats every 32 bytes
    }
    WireTranscript transcript;
    for (std::size_t chunk = 0; chunk < 3; ++chunk)
    {
        bytes::Bytes frame = {0x36};
        frame.insert(frame.end(), image.begin() + chunk * 32, image.begin() + (chunk + 1) * 32);
        transcript.push_back({.kind = WireTranscriptStep::Kind::kWrite, .frame = frame});
    }
    const std::string text = FormatWireTranscript(transcript, image);
    EXPECT_THAT(text, HasSubstr("{image 0x0+32}"));
    EXPECT_THAT(text, HasSubstr("{image 0x20+32}"));
    EXPECT_THAT(text, HasSubstr("{image 0x40+32}"));
}

} // namespace
} // namespace fastecu::flash
