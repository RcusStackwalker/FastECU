#include <gtest/gtest.h>
#include <gmock/gmock-matchers.h>

#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"

using namespace mutdma;

TEST(TestFreeform, size_descriptor_mapping)
{
    ASSERT_EQ(SizeToDescriptor(1), bytes::Byte(0));
    ASSERT_EQ(SizeToDescriptor(2), bytes::Byte(1));
    ASSERT_EQ(SizeToDescriptor(4), bytes::Byte(2));
}

TEST(TestFreeform, setup_frame)
{
    const MutDmaFrame f = BuildSetupFrame(0xA0, 3);
    ASSERT_EQ(static_cast<int>(f.size()), kFrameLen);
    ASSERT_EQ(f[0], bytes::Byte(0xA0));
    ASSERT_EQ(f[1], bytes::Byte(3)); // channel count
    ASSERT_EQ(f[kTrailerOffset], kTrailerFreeform);
    ASSERT_TRUE(VerifyFrame(f));
}

TEST(TestFreeform, reqlen_formula)
{
    ASSERT_EQ(ReqLen(1), std::size_t{((1U + 3U) >> 2U) + 1U * 2U + 0x1cU}); // 1 + 2 + 28 = 31
    ASSERT_EQ(ReqLen(4), std::size_t{((4U + 3U) >> 2U) + 4U * 2U + 0x1cU}); // 1 + 8 + 28 = 37
}

TEST(TestFreeform, id_list_frame)
{
    std::vector<Channel> ch = {{0x8000, 2}, {0x8004, 1}}; // N=2
    const bytes::Bytes f = BuildIdListFrame(0xA1, ch);
    ASSERT_EQ(f.size(), ReqLen(2));     // ((2+3)>>2)+4+28 = 1+4+28 = 33
    ASSERT_EQ(f[0], bytes::Byte(0xA1)); // rate selector
    ASSERT_EQ(f[1], bytes::Byte(2));    // count
    // descriptors: ch0=2B->1 at bits[7:6], ch1=1B->0 at bits[5:4] => 0b01000000 = 0x40
    ASSERT_EQ(f[2], bytes::Byte(0x40));
    // ids start at offset 2 + ceil(N/4) = 2 + 1 = 3, little-endian u16
    ASSERT_EQ(f[3], bytes::Byte(0x00));
    ASSERT_EQ(f[4], bytes::Byte(0x80));
    ASSERT_EQ(f[5], bytes::Byte(0x04));
    ASSERT_EQ(f[6], bytes::Byte(0x80));
    // checksum at reqLen-2 over bytes [0..reqLen-3]; trailer 0x0D at reqLen-1
    ASSERT_EQ(f[f.size() - 2], Sum8(f, 0, f.size() - 2));
    ASSERT_EQ(f[f.size() - 1], kTrailerStd);
}

TEST(TestFreeform, decode_stream_values)
{
    std::vector<Channel> ch = {{0x8000, 2}, {0x8004, 1}, {0x8008, 4}};
    ASSERT_EQ(ResponseDataLength(ch), std::size_t{2 + 1 + 4});            // 7
    const bytes::Bytes data = {0x34, 0x12, 0x56, 0xEF, 0xCD, 0xAB, 0x89}; // LE per channel
    std::vector<std::uint32_t> v = DecodeStreamValues(ch, data);
    ASSERT_EQ(v.size(), 3U);
    ASSERT_EQ(v.at(0), std::uint32_t(0x1234));
    ASSERT_EQ(v.at(1), std::uint32_t(0x56));
    ASSERT_EQ(v.at(2), std::uint32_t(0x89ABCDEF));
}

TEST(TestFreeform, decode_stream_values_rejects_incomplete_payload)
{
    const std::vector<Channel> channels = {{0x8000, 2}, {0x8004, 1}};
    EXPECT_TRUE(DecodeStreamValues(channels, bytes::Bytes{0x12}).empty());
}

TEST(TestFreeform, decode_stream_values_rejects_extra_payload)
{
    const std::vector<Channel> channels = {{0x8000, 2}};
    EXPECT_TRUE(DecodeStreamValues(channels, bytes::Bytes{0x12, 0x34, 0x56}).empty());
}

TEST(MutFreeform, RequestCodesMatchColtFirmwareByteAssembly)
{
    // 33520003 at 0x1164c-0x11660 computes first | (second << 8).
    const auto frame = BuildIdListFrame(0xa1, {{0x8123, 2}, {0x4567, 4}});
    ASSERT_EQ(frame.size(), 33U);
    EXPECT_EQ(frame[2], 0x60);
    EXPECT_EQ(frame[3], 0x23);
    EXPECT_EQ(frame[4], 0x81);
    EXPECT_EQ(frame[5], 0x67);
    EXPECT_EQ(frame[6], 0x45);
    EXPECT_EQ(frame[31], 0x53);
    EXPECT_EQ(frame[32], 0x0d);
}

TEST(MutFreeform, StreamValuesMatchColtFirmwareReversedMemoryBytes)
{
    // Two/four-byte big-endian memory sources are emitted src[1..0]/src[3..0].
    const auto values = DecodeStreamValues({{0x4010, 1}, {0x4020, 2}, {0x4030, 4}},
                                           bytes::Bytes{0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89});
    EXPECT_THAT(values, ::testing::ElementsAre(0x56U, 0x1234U, 0x89abcdefU));
}

TEST(MutFreeform, SingleByteBackedSourcesRemainZeroExtended)
{
    const auto values = DecodeStreamValues({{0x10, 2}, {0x11, 4}}, bytes::Bytes{0xab, 0, 0xcd, 0, 0, 0});
    EXPECT_THAT(values, ::testing::ElementsAre(0xabU, 0xcdU));
}
