#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/backend/protocol/mut_dma_driver.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_memory.h"
#include "src/backend/protocol/testing/scripted_kline_transport.h"

#include <array>
#include <string>
#include <vector>

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
    ASSERT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
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
    ASSERT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig)); // 0xFFF0 + 32 > 0x10000
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
    // 0x51 + one payload byte + checksum + trailer, for widths 2 and 1.
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

TEST(TestDriver, start_propagates_each_exchange_failure_and_stays_idle)
{
    const std::vector<Channel> ch = {{0x8000, 2}};
    const MutDmaFrame setup = BuildSetupFrame(0xA0, 1);
    const std::array<bytes::Bytes, 2> writes{bytes::Bytes(setup.begin(), setup.end()), BuildIdListFrame(0xA1, ch)};
    const std::array<MutDmaFrame, 2> acks{BuildCommandFrame(0xA5, bytes::Bytes{}, kTrailerStd),
                                          BuildCommandFrame(0x05, bytes::Bytes{}, kTrailerStd)};
    const std::array<std::string, 2> bad_ack_details{"MUT/DMA setup acknowledgement invalid",
                                                     "MUT/DMA channel-list acknowledgement invalid"};
    for (std::size_t faulted = 0; faulted < writes.size(); ++faulted)
    {
        SCOPED_TRACE(::testing::Message() << "exchange " << faulted);
        for (int fault = 0; fault < 4; ++fault)
        {
            SCOPED_TRACE(::testing::Message() << "fault " << fault);
            ScriptedKlineTransport t;
            for (std::size_t i = 0; i < faulted; ++i)
            {
                t.ExpectWrite(writes[i]);
                t.QueueRead(acks[i]);
            }
            if (fault != 0)
            {
                // Fault 0 leaves the write unexpected, so the transport rejects it.
                t.ExpectWrite(writes[faulted]);
            }
            switch (fault)
            {
            case 0:
                break;
            case 1:
                t.QueueError(fastecu::ErrorKind::kTimeout, "sentinel read failure");
                break;
            case 2:
                t.QueueNoFrame();
                break;
            default:
                // The other exchange's acknowledgement is well formed but wrong here.
                t.QueueRead(acks[1 - faulted]);
                break;
            }
            AlreadyInMode init(125000);
            MutDmaDriver d(t, init);
            fastecu::FakeCancellationToken cancellation;

            const auto started = d.StartFreeFormLog(ch, 0xA0, 0xA1, cancellation);
            if (fault == 0)
            {
                EXPECT_THAT(started, fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal,
                                                                 "unexpected scripted K-Line write"));
            }
            else if (fault == 1)
            {
                EXPECT_THAT(started,
                            fastecu::testing::IsErrWith(fastecu::ErrorKind::kTimeout, "sentinel read failure"));
            }
            else
            {
                EXPECT_THAT(started,
                            fastecu::testing::IsErrWith(fastecu::ErrorKind::kBadResponse, bad_ack_details[faulted]));
            }
            EXPECT_FALSE(d.IsStreaming());
            EXPECT_TRUE(t.ScriptConsumed());
        }
    }
}
