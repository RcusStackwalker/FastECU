#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/backend/protocol/mut_dma_driver.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"
#include "src/backend/protocol/testing/scripted_kline_transport.h"

using namespace mutdma;
using namespace std::chrono_literals;

namespace
{
class FailingInit : public mutdma::IMutDmaInit
{
  public:
    fastecu::Status Wake(mutdma::IKlineTransport&) override
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "sentinel init wake failure");
    }
};

} // namespace

TEST(TestDriver, free_form_handshake_reaches_streaming)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    // host setup (0xA0,N=1) -> ECU ACK-1 -> host id-list -> ECU ACK-2
    t.ExpectWrite(BuildSetupFrame(0xA0, 1));
    const MutDmaFrame ack1 = BuildCommandFrame(0xA5, bytes::Bytes{}, kTrailerStd);
    t.QueueRead(ack1);
    t.ExpectWrite(BuildIdListFrame(0xA1, ch));
    const MutDmaFrame ack2 = BuildCommandFrame(0x05, bytes::Bytes{}, kTrailerStd);
    t.QueueRead(ack2);
    MutDmaDriver d(t, init);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.StartFreeFormLog(ch, 0xA0, 0xA1, cancellation), fastecu::testing::IsOk());
    ASSERT_TRUE(d.IsStreaming());
    ASSERT_TRUE(t.ScriptConsumed());
}

TEST(TestDriver, write_memory_sends_and_acks)
{
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    const bytes::Bytes data = test_bytes::BytesFromHex("DEAD");
    const std::vector<MutDmaFrame> frames = BuildWriteFrames(0x8010, data);
    t.ExpectWrite(frames.at(0));
    t.QueueRead(BuildCommandFrame(0x87, bytes::Bytes{0x80, 0x00}, kTrailerStd)); // echo ack
    MutDmaDriver d(t, init);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.WriteMemory(0x8010, data, cancellation), fastecu::testing::IsOk());
    ASSERT_TRUE(t.ScriptConsumed());
}

TEST(TestDriver, poll_decodes_stream_frame)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    // one streamed frame: [0x51][34 12][csum][0x0D]
    bytes::Bytes fr = {0x51, 0x34, 0x12};
    fr.push_back(Sum8(fr));
    fr.push_back(kTrailerStd);
    t.QueueRead(fr);
    MutDmaDriver d(t, init);
    d.SetChannelsForTest(ch);
    fastecu::FakeCancellationToken cancellation;
    const auto result = d.PollOnce(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->size(), 1U);
    ASSERT_EQ(result->at(0), std::uint32_t(0x1234));
}

TEST(TestDriver, handshake_fails_on_wake_failure)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    FailingInit init;
    MutDmaDriver d(t, init);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.StartFreeFormLog(ch, 0xA0, 0xA1, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kBadResponse, "sentinel init wake failure"));
    ASSERT_FALSE(d.IsStreaming());
}

TEST(TestDriver, start_propagates_disconnected_set_baud_error_kind_and_detail)
{
    const std::vector<Channel> channels = {{0x8000, 2}};
    ScriptedKlineTransport transport;
    transport.QueueSetBaudError(fastecu::ErrorKind::kDisconnected, "sentinel set-baud disconnect");
    AlreadyInMode init(125000);
    MutDmaDriver driver(transport, init);
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(driver.StartFreeFormLog(channels, 0xA0, 0xA1, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "sentinel set-baud disconnect"));
}

TEST(TestDriver, start_propagates_internal_set_baud_error_kind_and_detail)
{
    const std::vector<Channel> channels = {{0x8000, 2}};
    ScriptedKlineTransport transport;
    transport.QueueSetBaudError(fastecu::ErrorKind::kInternal, "sentinel set-baud internal");
    AlreadyInMode init(125000);
    MutDmaDriver driver(transport, init);
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(driver.StartFreeFormLog(channels, 0xA0, 0xA1, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel set-baud internal"));
}

TEST(TestDriver, start_propagates_queued_write_error_kind_and_detail)
{
    const std::vector<Channel> channels = {{0x8000, 2}};
    ScriptedKlineTransport transport;
    transport.ExpectWrite(BuildSetupFrame(0xA0, 1));
    transport.QueueWriteError(fastecu::ErrorKind::kDisconnected, "sentinel setup write disconnect");
    AlreadyInMode init(125000);
    MutDmaDriver driver(transport, init);
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(driver.StartFreeFormLog(channels, 0xA0, 0xA1, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "sentinel setup write disconnect"));
}

TEST(TestDriver, start_propagates_queued_read_error_kind_and_detail)
{
    const std::vector<Channel> channels = {{0x8000, 2}};
    ScriptedKlineTransport transport;
    transport.ExpectWrite(BuildSetupFrame(0xA0, 1));
    transport.QueueError(fastecu::ErrorKind::kInternal, "sentinel setup read internal");
    AlreadyInMode init(125000);
    MutDmaDriver driver(transport, init);
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(driver.StartFreeFormLog(channels, 0xA0, 0xA1, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel setup read internal"));
}

TEST(TestDriver, handshake_fails_on_bad_ack)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    t.ExpectWrite(BuildSetupFrame(0xA0, 1));
    // wrong ACK-1 opcode (0x00 instead of 0xA5/0xB5), though checksum is valid
    t.QueueRead(BuildCommandFrame(0x00, bytes::Bytes{}, kTrailerStd));
    MutDmaDriver d(t, init);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.StartFreeFormLog(ch, 0xA0, 0xA1, cancellation),
                fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
    ASSERT_FALSE(d.IsStreaming());
}

TEST(TestDriver, poll_reports_bad_response_on_invalid_checksum)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    const bytes::Bytes bad = {0x51, 0x12, 0x34, 0x00, kTrailerStd}; // wrong checksum
    t.QueueRead(bad);
    MutDmaDriver d(t, init);
    d.SetChannelsForTest(ch);
    fastecu::FakeCancellationToken cancellation;
    const auto result = d.PollOnce(50ms, cancellation);
    EXPECT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(TestDriver, write_memory_fails_on_bad_echo)
{
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    const bytes::Bytes data = test_bytes::BytesFromHex("DEAD");
    t.ExpectWrite(BuildWriteFrames(0x8010, data).at(0));
    MutDmaFrame bad_echo = BuildCommandFrame(0x87, bytes::Bytes{0x80, 0x00}, kTrailerStd);
    bad_echo[49] = static_cast<bytes::Byte>(bad_echo[49] ^ 0xFF); // corrupt checksum
    t.QueueRead(bad_echo);
    MutDmaDriver d(t, init);
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(d.WriteMemory(0x8010, data, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(TestDriver, write_memory_rejects_overflow)
{
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    MutDmaDriver d(t, init);
    const bytes::Bytes data(32, 0x5A);
    fastecu::FakeCancellationToken cancellation;
    const auto result = d.WriteMemory(0xFFF0, data, cancellation);
    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOk())); // 0xFFF0 + 32 > 0x10000
    EXPECT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(TestDriver, handshake_propagates_cancellation_from_bounded_read)
{
    std::vector<Channel> ch = {{0x8000, 2}};
    ScriptedKlineTransport t;
    AlreadyInMode init(125000);
    t.ExpectWrite(BuildSetupFrame(0xA0, 1));
    t.QueueRead(BuildCommandFrame(0xA5, bytes::Bytes{}, kTrailerStd));
    MutDmaDriver d(t, init);
    fastecu::FakeCancellationToken cancellation(true);

    ASSERT_THAT(d.StartFreeFormLog(ch, 0xA0, 0xA1, cancellation),
                fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(TestDriver, ChecksumValidShortStreamDoesNotFabricateZeroReadings)
{
    ScriptedKlineTransport transport;
    AlreadyInMode init(125000);
    // Valid checksum for one data byte; the selected widths require three.
    transport.QueueRead(bytes::Bytes{0x51, 0x12, 0x63, 0x0d});
    MutDmaDriver driver(transport, init);
    driver.SetChannelsForTest({{0x4010, 2}, {0x4020, 1}});
    fastecu::FakeCancellationToken cancellation;
    EXPECT_THAT(driver.PollOnce(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(TestDriver, ChecksumValidOversizedStreamIsRejected)
{
    ScriptedKlineTransport transport;
    AlreadyInMode init(125000);
    transport.QueueRead(bytes::Bytes{0x51, 0x12, 0x34, 0x56, 0xed, 0x0d});
    MutDmaDriver driver(transport, init);
    driver.SetChannelsForTest({{0x4010, 2}});
    fastecu::FakeCancellationToken cancellation;
    EXPECT_THAT(driver.PollOnce(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}
