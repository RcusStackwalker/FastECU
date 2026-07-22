#include <gtest/gtest.h>

#include "byte_test_utils.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/backend/protocol/mut_dma_driver.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"
#include "scripted_kline_transport.h"

using namespace mutdma;

namespace
{
class FailingInit : public mutdma::IMutDmaInit
{
  public:
    bool wake(mutdma::IKlineTransport&) override
    {
        return false;
    }
};

class NeverCancelled final : public fastecu::ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

class Cancelled final : public fastecu::ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return true;
    }
};
} // namespace

TEST(TestDriver, free_form_handshake_reaches_streaming)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    // host setup (0xA0,N=1) -> ECU ACK-1 -> host id-list -> ECU ACK-2
    t.expectWrite(buildSetupFrame(0xA0, 1));
    const MutDmaFrame ack1 = buildCommandFrame(0xA5, bytes::Bytes{}, TRAILER_STD);
    t.queueRead(ack1);
    t.expectWrite(buildIdListFrame(0xA1, ch));
    const MutDmaFrame ack2 = buildCommandFrame(0x05, bytes::Bytes{}, TRAILER_STD);
    t.queueRead(ack2);
    MutDmaDriver d(t, init);
    NeverCancelled cancellation;
    ASSERT_TRUE(d.startFreeFormLog(ch, 0xA0, 0xA1, cancellation));
    ASSERT_TRUE(d.isStreaming());
    ASSERT_TRUE(t.scriptConsumed());
}

TEST(TestDriver, write_memory_sends_and_acks)
{
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    const bytes::Bytes data = test_bytes::bytesFromHex("DEAD");
    const std::vector<MutDmaFrame> frames = buildWriteFrames(0x8010, data);
    t.expectWrite(frames.at(0));
    t.queueRead(buildCommandFrame(0x87, bytes::Bytes{0x80, 0x00}, TRAILER_STD)); // echo ack
    MutDmaDriver d(t, init);
    NeverCancelled cancellation;
    ASSERT_TRUE(d.writeMemory(0x8010, data, cancellation));
    ASSERT_TRUE(t.scriptConsumed());
}

TEST(TestDriver, poll_decodes_stream_frame)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    // one streamed frame: [0x51][12 34][csum][0x0D]
    bytes::Bytes fr = {0x51, 0x12, 0x34};
    fr.push_back(sum8(fr));
    fr.push_back(TRAILER_STD);
    t.queueRead(fr);
    MutDmaDriver d(t, init);
    d.setChannelsForTest(ch);
    NeverCancelled cancellation;
    const auto result = d.pollOnce(50, cancellation);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->size(), 1u);
    ASSERT_EQ(result->at(0), std::uint32_t(0x1234));
}

TEST(TestDriver, handshake_fails_on_wake_failure)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    FailingInit init;
    MutDmaDriver d(t, init);
    NeverCancelled cancellation;
    const auto result = d.startFreeFormLog(ch, 0xA0, 0xA1, cancellation);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
    ASSERT_FALSE(d.isStreaming());
}

TEST(TestDriver, handshake_fails_on_bad_ack)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    t.expectWrite(buildSetupFrame(0xA0, 1));
    // wrong ACK-1 opcode (0x00 instead of 0xA5/0xB5), though checksum is valid
    t.queueRead(buildCommandFrame(0x00, bytes::Bytes{}, TRAILER_STD));
    MutDmaDriver d(t, init);
    NeverCancelled cancellation;
    const auto result = d.startFreeFormLog(ch, 0xA0, 0xA1, cancellation);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
    ASSERT_FALSE(d.isStreaming());
}

TEST(TestDriver, poll_returns_empty_on_bad_frame)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    const bytes::Bytes bad = {0x51, 0x12, 0x34, 0x00, TRAILER_STD}; // wrong checksum
    t.queueRead(bad);
    MutDmaDriver d(t, init);
    d.setChannelsForTest(ch);
    NeverCancelled cancellation;
    const auto result = d.pollOnce(50, cancellation);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->empty());
}

TEST(TestDriver, write_memory_fails_on_bad_echo)
{
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    const bytes::Bytes data = test_bytes::bytesFromHex("DEAD");
    t.expectWrite(buildWriteFrames(0x8010, data).at(0));
    MutDmaFrame badEcho = buildCommandFrame(0x87, bytes::Bytes{0x80, 0x00}, TRAILER_STD);
    badEcho[49] = static_cast<bytes::Byte>(badEcho[49] ^ 0xFF); // corrupt checksum
    t.queueRead(badEcho);
    MutDmaDriver d(t, init);
    NeverCancelled cancellation;
    const auto result = d.writeMemory(0x8010, data, cancellation);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
}

TEST(TestDriver, write_memory_rejects_overflow)
{
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    MutDmaDriver d(t, init);
    const bytes::Bytes data(32, 0x5A);
    NeverCancelled cancellation;
    const auto result = d.writeMemory(0xFFF0, data, cancellation);
    ASSERT_FALSE(result); // 0xFFF0 + 32 > 0x10000
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(TestDriver, handshake_propagates_cancellation_from_bounded_read)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    t.expectWrite(buildSetupFrame(0xA0, 1));
    t.queueRead(buildCommandFrame(0xA5, bytes::Bytes{}, TRAILER_STD));
    MutDmaDriver d(t, init);
    Cancelled cancellation;

    const auto result = d.startFreeFormLog(ch, 0xA0, 0xA1, cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
}
