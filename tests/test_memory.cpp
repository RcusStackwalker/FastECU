#include <gtest/gtest.h>

#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"

using namespace mutdma;

TEST(TestMemory, write_frame_single)
{
    const bytes::Bytes data = {0xDE, 0xAD};
    const std::vector<MutDmaFrame> frames = buildWriteFrames(0x8010, data);
    ASSERT_EQ(frames.size(), std::size_t(1));
    const MutDmaFrame& f = frames.at(0);
    ASSERT_EQ(static_cast<int>(f.size()), FRAME_LEN);
    ASSERT_EQ(f[0], bytes::Byte(0x87)); // cmd
    ASSERT_EQ(f[1], bytes::Byte(0x00)); // sub-selector hi (0x0003)
    ASSERT_EQ(f[2], bytes::Byte(0x03)); // sub-selector lo = write arbitrary
    ASSERT_EQ(f[3], bytes::Byte(0x80)); // addr hi
    ASSERT_EQ(f[4], bytes::Byte(0x10)); // addr lo
    ASSERT_EQ(f[5], bytes::Byte(0x02)); // size
    ASSERT_EQ(f[6], bytes::Byte(0xDE)); // data
    ASSERT_EQ(f[7], bytes::Byte(0xAD));
    ASSERT_TRUE(verifyFrame(f));
}

TEST(TestMemory, write_chunks_large_payload)
{
    bytes::Bytes data(100, 0x5A); // > one frame's data capacity
    const std::vector<MutDmaFrame> frames = buildWriteFrames(0x8000, data);
    ASSERT_TRUE(frames.size() >= 3);
    int total = 0;
    for (const MutDmaFrame& f : frames)
    {
        total += f[5];
    }
    ASSERT_EQ(total, static_cast<int>(data.size())); // all bytes accounted for
}

TEST(TestMemory, read_plan_one_byte_channels)
{
    QVector<Channel> ch = planReadChannels(0x8000, 3);
    ASSERT_EQ(ch.size(), 3);
    ASSERT_EQ(ch.at(0).id, std::uint16_t(0x8000));
    ASSERT_EQ(ch.at(0).len, bytes::Byte(1));
    ASSERT_EQ(ch.at(2).id, std::uint16_t(0x8002));
}

TEST(TestMemory, read_reassembles_values)
{
    QVector<std::uint32_t> vals = {0xDE, 0xAD, 0xBE};
    const bytes::Bytes out = reassembleRead(vals);
    const bytes::Bytes expected = {0xDE, 0xAD, 0xBE};
    ASSERT_TRUE(out == expected);
}

TEST(TestMemory, write_frames_rejectEmptyAndOverflowInputs)
{
    ASSERT_TRUE(buildWriteFrames(0x8000, bytes::Bytes{}).empty());
    ASSERT_TRUE(buildWriteFrames(0xFFF0, bytes::Bytes(32, 0x5A)).empty());
}
