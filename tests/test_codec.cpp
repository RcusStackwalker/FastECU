#include <gtest/gtest.h>

#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"

using namespace mutdma;

TEST(TestCodec, sum8_wraps)
{
    const bytes::Bytes b = {0x01, 0x02, 0x03, 0x04};
    ASSERT_EQ(sum8(b), bytes::Byte(0x0A));
    const bytes::Bytes big = {0xFF, 0xFF, 0xFF}; // 0x2FD -> 0xFD
    ASSERT_EQ(sum8(big), bytes::Byte(0xFD));
}

TEST(TestCodec, command_frame_layout)
{
    const bytes::Bytes payload = {0xAA, 0xBB};
    const MutDmaFrame f = buildCommandFrame(0x81, payload, TRAILER_STD);
    ASSERT_EQ(static_cast<int>(f.size()), FRAME_LEN); // 51
    ASSERT_EQ(f[0], bytes::Byte(0x81));               // cmd
    ASSERT_EQ(f[1], bytes::Byte(0xAA));               // payload
    ASSERT_EQ(f[2], bytes::Byte(0xBB));
    ASSERT_EQ(f[3], bytes::Byte(0x00)); // zero pad
    ASSERT_EQ(f[49], sum8(f, 0, 49));   // checksum over bytes 0..48
    ASSERT_EQ(f[50], TRAILER_STD);
}

TEST(TestCodec, verify_accepts_built_frame)
{
    MutDmaFrame f = buildCommandFrame(0xA0, bytes::Bytes{0x04}, TRAILER_FREEFORM);
    ASSERT_TRUE(verifyFrame(f));
    f[49] = static_cast<bytes::Byte>(f[49] ^ 0xFF); // corrupt checksum
    ASSERT_FALSE(verifyFrame(f));
}

TEST(TestCodec, stream_frame_parse)
{
    // [LOG_ID][data...][sum8(id..lastData)][0x0D]
    const bytes::Bytes data = {0x12, 0x34, 0x00, 0x56};
    bytes::Bytes f = {0x51};
    f.insert(f.end(), data.begin(), data.end());
    f.push_back(sum8(f));
    f.push_back(TRAILER_STD);
    StreamFrame s = parseStreamFrame(f);
    ASSERT_TRUE(s.ok);
    ASSERT_EQ(s.logId, bytes::Byte(0x51));
    ASSERT_TRUE(s.data == data);
    f[f.size() - 2] = static_cast<bytes::Byte>(f[f.size() - 2] ^ 0xFF); // corrupt checksum
    ASSERT_FALSE(parseStreamFrame(f).ok);
}

TEST(TestCodec, verify_rejectsWrongLengthAndTrailer)
{
    ASSERT_FALSE(verifyFrame(bytes::Bytes{}));
    MutDmaFrame frame = buildCommandFrame(0x81, bytes::Bytes{}, TRAILER_STD);
    frame[TRAILER_OFFSET] = 0x00;
    ASSERT_FALSE(verifyFrame(frame));
}

TEST(TestCodec, stream_frame_rejectsTruncatedAndWrongTrailer)
{
    ASSERT_FALSE(parseStreamFrame(bytes::Bytes{0x51, 0x51}).ok);
    ASSERT_FALSE(parseStreamFrame(bytes::Bytes{0x51, 0x51, 0x00}).ok);
}
