#include <gtest/gtest.h>

#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"

using namespace mutdma;

TEST(TestFreeform, size_descriptor_mapping)
{
    ASSERT_EQ(sizeToDescriptor(1), bytes::Byte(0));
    ASSERT_EQ(sizeToDescriptor(2), bytes::Byte(1));
    ASSERT_EQ(sizeToDescriptor(4), bytes::Byte(2));
}

TEST(TestFreeform, setup_frame)
{
    const MutDmaFrame f = buildSetupFrame(0xA0, 3);
    ASSERT_EQ(static_cast<int>(f.size()), FRAME_LEN);
    ASSERT_EQ(f[0], bytes::Byte(0xA0));
    ASSERT_EQ(f[1], bytes::Byte(3)); // channel count
    ASSERT_EQ(f[TRAILER_OFFSET], TRAILER_FREEFORM);
    ASSERT_TRUE(verifyFrame(f));
}

TEST(TestFreeform, reqlen_formula)
{
    ASSERT_EQ(reqLen(1), ((1 + 3) >> 2) + 1 * 2 + 0x1c); // 1 + 2 + 28 = 31
    ASSERT_EQ(reqLen(4), ((4 + 3) >> 2) + 4 * 2 + 0x1c); // 1 + 8 + 28 = 37
}

TEST(TestFreeform, id_list_frame)
{
    std::vector<Channel> ch = {{0x8000, 2}, {0x8004, 1}}; // N=2
    const bytes::Bytes f = buildIdListFrame(0xA1, ch);
    ASSERT_EQ(static_cast<int>(f.size()), reqLen(2)); // ((2+3)>>2)+4+28 = 1+4+28 = 33
    ASSERT_EQ(f[0], bytes::Byte(0xA1));               // rate selector
    ASSERT_EQ(f[1], bytes::Byte(2));                  // count
    // descriptors: ch0=2B->1 at bits[7:6], ch1=1B->0 at bits[5:4] => 0b01000000 = 0x40
    ASSERT_EQ(f[2], bytes::Byte(0x40));
    // ids start at offset 2 + ceil(N/4) = 2 + 1 = 3, big-endian u16
    ASSERT_EQ(f[3], bytes::Byte(0x80));
    ASSERT_EQ(f[4], bytes::Byte(0x00));
    ASSERT_EQ(f[5], bytes::Byte(0x80));
    ASSERT_EQ(f[6], bytes::Byte(0x04));
    // checksum at reqLen-2 over bytes [0..reqLen-3]; trailer 0x0D at reqLen-1
    ASSERT_EQ(f[f.size() - 2], sum8(f, 0, f.size() - 2));
    ASSERT_EQ(f[f.size() - 1], TRAILER_STD);
}

TEST(TestFreeform, decode_stream_values)
{
    std::vector<Channel> ch = {{0x8000, 2}, {0x8004, 1}, {0x8008, 4}};
    ASSERT_EQ(responseDataLength(ch), 2 + 1 + 4);                         // 7
    const bytes::Bytes data = {0x12, 0x34, 0x56, 0x89, 0xAB, 0xCD, 0xEF}; // BE per channel
    std::vector<std::uint32_t> v = decodeStreamValues(ch, data);
    ASSERT_EQ(v.size(), 3);
    ASSERT_EQ(v.at(0), std::uint32_t(0x1234));
    ASSERT_EQ(v.at(1), std::uint32_t(0x56));
    ASSERT_EQ(v.at(2), std::uint32_t(0x89ABCDEF));
}

TEST(TestFreeform, decode_stream_values_zeroFillsMissingBytesCompatibility)
{
    const std::vector<Channel> channels = {{0x8000, 2}, {0x8004, 1}};
    const std::vector<std::uint32_t> values = decodeStreamValues(channels, bytes::Bytes{0x12});
    ASSERT_EQ(values, std::vector<std::uint32_t>({0x12, 0x00}));
}
