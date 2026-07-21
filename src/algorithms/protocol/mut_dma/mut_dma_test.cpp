#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"

#include <gtest/gtest.h>

using namespace mutdma;

// Mirrors tests/test_codec.cpp's TestCodec.sum8_wraps -- same input/output vectors,
// exercised again here so the mut_dma package's own test target proves the codec
// entry point round-trips without depending on //tests.
TEST(MutDmaPortable, Sum8Wraps)
{
    const bytes::Bytes b = {0x01, 0x02, 0x03, 0x04};
    EXPECT_EQ(sum8(b), bytes::Byte(0x0A));
    const bytes::Bytes big = {0xFF, 0xFF, 0xFF}; // 0x2FD -> 0xFD
    EXPECT_EQ(sum8(big), bytes::Byte(0xFD));
}

// Mirrors tests/test_freeform.cpp's TestFreeform.id_list_frame vector, expressed
// with std::vector<Channel> instead of QVector<Channel>.
TEST(MutDmaPortable, BuildIdListFrameEncodesChannelsAndTrailer)
{
    const std::vector<Channel> ch = {{0x8000, 2}, {0x8004, 1}}; // N=2
    const bytes::Bytes f = buildIdListFrame(0xA1, ch);
    ASSERT_EQ(static_cast<int>(f.size()), reqLen(2)); // ((2+3)>>2)+4+28 = 1+4+28 = 33
    EXPECT_EQ(f[0], bytes::Byte(0xA1));               // rate selector
    EXPECT_EQ(f[1], bytes::Byte(2));                  // count
    // descriptors: ch0=2B->1 at bits[7:6], ch1=1B->0 at bits[5:4] => 0b01000000 = 0x40
    EXPECT_EQ(f[2], bytes::Byte(0x40));
    // ids start at offset 2 + ceil(N/4) = 2 + 1 = 3, big-endian u16
    EXPECT_EQ(f[3], bytes::Byte(0x80));
    EXPECT_EQ(f[4], bytes::Byte(0x00));
    EXPECT_EQ(f[5], bytes::Byte(0x80));
    EXPECT_EQ(f[6], bytes::Byte(0x04));
    EXPECT_EQ(f[f.size() - 2], sum8(f, 0, f.size() - 2));
    EXPECT_EQ(f[f.size() - 1], TRAILER_STD);
}

// Mirrors tests/test_freeform.cpp's TestFreeform.decode_stream_values -- same
// three-channel, 7-byte big-endian payload, expressed with std::vector.
TEST(MutDmaPortable, DecodeStreamValuesRoundTripsChannelSizes)
{
    const std::vector<Channel> ch = {{0x8000, 2}, {0x8004, 1}, {0x8008, 4}};
    EXPECT_EQ(responseDataLength(ch), 2 + 1 + 4);                         // 7
    const bytes::Bytes data = {0x12, 0x34, 0x56, 0x89, 0xAB, 0xCD, 0xEF}; // BE per channel
    const std::vector<std::uint32_t> v = decodeStreamValues(ch, data);
    ASSERT_EQ(v.size(), std::size_t(3));
    EXPECT_EQ(v.at(0), std::uint32_t(0x1234));
    EXPECT_EQ(v.at(1), std::uint32_t(0x56));
    EXPECT_EQ(v.at(2), std::uint32_t(0x89ABCDEF));
}

// Mirrors tests/test_memory.cpp's TestMemory.write_frame_single -- same one-frame
// write encode.
TEST(MutDmaPortable, BuildWriteFramesEncodesSingleChunk)
{
    const bytes::Bytes data = {0xDE, 0xAD};
    const std::vector<MutDmaFrame> frames = buildWriteFrames(0x8010, data);
    ASSERT_EQ(frames.size(), std::size_t(1));
    const MutDmaFrame& f = frames.at(0);
    ASSERT_EQ(static_cast<int>(f.size()), FRAME_LEN);
    EXPECT_EQ(f[0], bytes::Byte(0x87)); // cmd
    EXPECT_EQ(f[1], bytes::Byte(0x00)); // sub-selector hi (0x0003)
    EXPECT_EQ(f[2], bytes::Byte(0x03)); // sub-selector lo = write arbitrary
    EXPECT_EQ(f[3], bytes::Byte(0x80)); // addr hi
    EXPECT_EQ(f[4], bytes::Byte(0x10)); // addr lo
    EXPECT_EQ(f[5], bytes::Byte(0x02)); // size
    EXPECT_EQ(f[6], bytes::Byte(0xDE)); // data
    EXPECT_EQ(f[7], bytes::Byte(0xAD));
    EXPECT_TRUE(verifyFrame(f));
}

// Mirrors tests/test_memory.cpp's TestMemory.read_plan_one_byte_channels /
// read_reassembles_values decode round-trip, expressed with std::vector.
TEST(MutDmaPortable, PlanReadChannelsAndReassembleRoundTrip)
{
    const std::vector<Channel> ch = planReadChannels(0x8000, 3);
    ASSERT_EQ(ch.size(), std::size_t(3));
    EXPECT_EQ(ch.at(0).id, std::uint16_t(0x8000));
    EXPECT_EQ(ch.at(0).len, bytes::Byte(1));
    EXPECT_EQ(ch.at(2).id, std::uint16_t(0x8002));

    const std::vector<std::uint32_t> vals = {0xDE, 0xAD, 0xBE};
    const bytes::Bytes out = reassembleRead(vals);
    const bytes::Bytes expected = {0xDE, 0xAD, 0xBE};
    EXPECT_EQ(out, expected);
}
