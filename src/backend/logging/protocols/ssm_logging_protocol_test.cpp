#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "src/backend/protocol/testing/scripted_ssm_transport.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"

namespace
{
using namespace std::chrono_literals;
using fastecu::logging::LoggingChannel;
using fastecu::logging::RawAssembly;
using fastecu::logging::SsmLoggingProtocol;

LoggingChannel Channel(std::string id = "rpm", std::uint32_t address = 0x1000, std::size_t length = 1)
{
    return LoggingChannel{
        .id = std::move(id),
        .address = address,
        .length = length,
        .raw_assembly = RawAssembly::kDecimalBytesConcatenated,
        .from_byte_expression = "x",
        .unit = "rpm",
        .decimal_precision = 0,
    };
}

bytes::Bytes BuildRequest(bytes::ByteView payload, bool target_is_ecu = true)
{
    return ssm_protocol::AddHeader(payload, 0xF0, target_is_ecu ? 0x10 : 0x18);
}

bytes::Bytes BuildResponse(bytes::ByteView payload)
{
    bytes::Bytes message = {0x80, 0xf0, 0x10, static_cast<bytes::Byte>(payload.size() + 1), 0xe8};
    message.insert(message.end(), payload.begin(), payload.end());
    message.push_back(bytes::Sum8(message));
    return message;
}

void QueueNoFrames(ScriptedSsmTransport& transport, int count)
{
    for (int i = 0; i < count; ++i)
    {
        transport.QueueNoFrame();
    }
}
} // namespace

TEST(SsmLoggingProtocolTest, StartPreservesHistoricalRequestVector)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->QueueRead(BuildResponse(bytes::Bytes{7}));
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Start(cancellation), fastecu::testing::IsOk());
    EXPECT_TRUE(script->ScriptConsumed());
    EXPECT_TRUE(script->Ok());
}

TEST(SsmLoggingProtocolTest, StartReturnsBadResponseForShortReply)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->QueueRead(bytes::Bytes{});
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, true);

    ASSERT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(SsmLoggingProtocolTest, StartReturnsBadResponseForNegativeReply)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    auto response = BuildResponse(bytes::Bytes{7});
    response[4] = 0x7f;
    response.back() = bytes::Sum8(bytes::ByteView(response).first(response.size() - 1));
    transport->QueueRead(response);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, true);

    ASSERT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(SsmLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->SetOpen(false);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "adapter disconnected"));
}

TEST(SsmLoggingProtocolTest, PreservesDecimalByteConcatenation)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueRead(BuildResponse(bytes::Bytes{16, 16}));
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel("rpm", 0x1000, 2)}, true, false);

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "rpm");
    EXPECT_EQ(result->samples[0].raw_value, "1616");
    EXPECT_TRUE(script->ScriptConsumed());
    EXPECT_TRUE(script->Ok());
}

TEST(SsmLoggingProtocolTest, PollPreservesChannelRequestOrder)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
    transport->QueueRead(BuildResponse(bytes::Bytes{42, 99}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel("first", 0x1000), Channel("second", 0x1003)},
                                true, false);

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2U);
    EXPECT_EQ(result->samples[0].channel_id, "first");
    EXPECT_EQ(result->samples[0].raw_value, "42");
    EXPECT_EQ(result->samples[1].channel_id, "second");
    EXPECT_EQ(result->samples[1].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, PollHonorsSnapshottedHistoricalResponseOffsets)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
    transport->QueueRead(BuildResponse(bytes::Bytes{42, 77, 99}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel("first", 0x1000), Channel("third", 0x1003)},
                                std::vector<std::size_t>{0, 2}, true, false);

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2U);
    EXPECT_EQ(result->samples[0].raw_value, "42");
    EXPECT_EQ(result->samples[1].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, PollReturnsNoResponseOnDirectReadTimeout)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, true);

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(SsmLoggingProtocolTest, HeaderResynchronizationRemainsDeadlineBounded)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueRead(bytes::Bytes(64, 0x01));
    QueueNoFrames(*transport, 8);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    const auto result = protocol.Poll(100ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
}

TEST(SsmLoggingProtocolTest, CancellationDuringFramingReturnsCancelled)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueRead(bytes::Bytes{0x80});
    transport->QueueRead(bytes::Bytes{0xf0, 0x10});
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(7);
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(SsmLoggingProtocolTest, StartCancellationReturnsCancelledWithoutIo)
{
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation(true);
    SsmLoggingProtocol protocol(clock, std::make_unique<ScriptedSsmTransport>(), {Channel()}, true, false);

    ASSERT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(SsmLoggingProtocolTest, PollPropagatesTypedWriteFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueWriteError(fastecu::ErrorKind::kDisconnected, "sentinel SSM write disconnect");
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "sentinel SSM write disconnect"));
}

TEST(SsmLoggingProtocolTest, PollPropagatesTypedReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueError(fastecu::ErrorKind::kInternal, "sentinel SSM read failure");
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel SSM read failure"));
}

TEST(SsmLoggingProtocolTest, StartPropagatesTypedWriteFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->QueueWriteError(fastecu::ErrorKind::kDisconnected, "sentinel SSM start write disconnect");
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "sentinel SSM start write disconnect"));
}

TEST(SsmLoggingProtocolTest, StartPropagatesTypedReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->QueueError(fastecu::ErrorKind::kInternal, "sentinel SSM start read failure");
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel SSM start read failure"));
}

TEST(SsmLoggingProtocolTest, PollPropagatesOpenPort2DirectReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueError(fastecu::ErrorKind::kDisconnected, "sentinel OpenPort2 direct read failure");
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, true);

    ASSERT_THAT(
        protocol.Poll(50ms, cancellation),
        fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "sentinel OpenPort2 direct read failure"));
}

TEST(SsmLoggingProtocolTest, HeaderResynchronizationPropagatesReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    // Enough bytes to leave the initial accumulation loop, but not framed as
    // an SSM response (0x80 0xf0 0x10), so resynchronization must shift and
    // re-read -- and that re-read fails.
    transport->QueueRead(bytes::Bytes{0x01, 0x02, 0x03});
    transport->QueueError(fastecu::ErrorKind::kInternal, "sentinel SSM resync read failure");
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel SSM resync read failure"));
}

TEST(SsmLoggingProtocolTest, FinalReadAfterHeaderMatchPropagatesReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    // Exactly a matching header: the accumulation loop stops (size >= 3), the
    // resynchronization loop is a no-op (header already matches), so the
    // trailing "read remaining payload" call executes -- and that read fails.
    transport->QueueRead(bytes::Bytes{0x80, 0xf0, 0x10});
    transport->QueueError(fastecu::ErrorKind::kInternal, "sentinel SSM final read failure");
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel SSM final read failure"));
}

TEST(SsmLoggingProtocolTest, PollCancellationReturnsCancelledWithoutIo)
{
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation(true);
    SsmLoggingProtocol protocol(clock, std::make_unique<ScriptedSsmTransport>(), {Channel()}, true, false);

    ASSERT_THAT(protocol.Poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(SsmLoggingProtocolTest, PollFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->SetOpen(false);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "adapter disconnected"));
}

TEST(SsmLoggingProtocolTest, PollSkipsChannelWhenResponseOffsetBeyondPayload)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
    // Payload is only 2 bytes long; the "second" channel's offset (5) lies
    // beyond it and must be skipped entirely rather than read out of bounds.
    transport->QueueRead(BuildResponse(bytes::Bytes{42, 99}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel("first", 0x1000), Channel("second", 0x1003)},
                                std::vector<std::size_t>{0, 5}, true, false);

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "first");
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

TEST(SsmLoggingProtocolTest, PollTruncatesRawValueWhenLengthExtendsBeyondPayload)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    // Channel wants 3 bytes starting at offset 0, but the payload is only 2
    // bytes long -- the raw value must be built from just what's available.
    transport->QueueRead(BuildResponse(bytes::Bytes{7, 8}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel("rpm", 0x1000, 3)}, true, false);

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "rpm");
    EXPECT_EQ(result->samples[0].raw_value, "78");
}

TEST(SsmLoggingProtocolTest, StopSucceeds)
{
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    SsmLoggingProtocol protocol(clock, std::make_unique<ScriptedSsmTransport>(), {Channel()}, true, false);

    EXPECT_THAT(protocol.Stop(), fastecu::testing::IsOk());
}

// A complete frame must not consume the next response, even when both arrive
// in one transport read. Replacing extraction with whole-buffer consumption
// loses the second sample and must fail this test.
TEST(SsmLoggingProtocolTest, DirectPollRetainsCoalescedFrameForNextPoll)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    const auto request = BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00});
    transport->ExpectWrite(request);
    transport->ExpectWrite(request);
    auto responses = BuildResponse(bytes::Bytes{42});
    const auto second = BuildResponse(bytes::Bytes{99});
    responses.insert(responses.end(), second.begin(), second.end());
    transport->QueueRead(responses);
    QueueNoFrames(*transport, 60);
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    const auto first = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_TRUE(first->responded);
    ASSERT_EQ(first->samples.size(), 1U);
    EXPECT_EQ(first->samples[0].raw_value, "42");
    const auto next = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(next, fastecu::testing::IsOk());
    ASSERT_TRUE(next->responded);
    ASSERT_EQ(next->samples.size(), 1U);
    EXPECT_EQ(next->samples[0].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, DirectPollLeavesConsecutiveTransportFramesForNextPoll)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    const auto request = BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00});
    transport->ExpectWrite(request);
    transport->ExpectWrite(request);
    transport->QueueRead(BuildResponse(bytes::Bytes{42}));
    transport->QueueRead(BuildResponse(bytes::Bytes{99}));
    QueueNoFrames(*transport, 60);
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    const auto first = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_TRUE(first->responded);
    const auto next = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(next, fastecu::testing::IsOk());
    ASSERT_TRUE(next->responded);
    ASSERT_EQ(next->samples.size(), 1U);
    EXPECT_EQ(next->samples[0].raw_value, "99");
}

class SsmFrameIntegrityTest : public ::testing::TestWithParam<bool>
{
};

TEST_P(SsmFrameIntegrityTest, StartRejectsCorruptChecksum)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    auto reply = BuildResponse(bytes::Bytes{7});
    reply.back() ^= 1;
    transport->QueueRead(reply);
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, GetParam());
    EXPECT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST_P(SsmFrameIntegrityTest, StartRejectsExtraProbeData)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->QueueRead(BuildResponse(bytes::Bytes{7, 8}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, GetParam());
    EXPECT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST_P(SsmFrameIntegrityTest, PollRejectsCorruptChecksumAndRetries)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    const auto request = BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00});
    transport->ExpectWrite(request);
    transport->ExpectWrite(request);
    auto reply = BuildResponse(bytes::Bytes{42});
    reply.back() ^= 1;
    transport->QueueRead(reply);
    // A direct transport may have a gap before the retry frame arrives.
    if (!GetParam())
    {
        transport->QueueNoFrame();
    }
    transport->QueueRead(BuildResponse(bytes::Bytes{99}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, GetParam());
    const auto rejected = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(rejected, fastecu::testing::IsOk());
    EXPECT_FALSE(rejected->responded);
    EXPECT_TRUE(rejected->samples.empty());
    const auto retry = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(retry, fastecu::testing::IsOk());
    ASSERT_TRUE(retry->responded);
    ASSERT_EQ(retry->samples.size(), 1U);
    EXPECT_EQ(retry->samples[0].raw_value, "99");
}

TEST_P(SsmFrameIntegrityTest, PollAcceptsCapturedTcuSender)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}, false));
    auto reply = BuildResponse(bytes::Bytes{42});
    reply[2] = 0x18;
    reply.back() = bytes::Sum8(bytes::ByteView(reply).first(reply.size() - 1));
    transport->QueueRead(reply);
    QueueNoFrames(*transport, 60);
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, false, GetParam());
    const auto result = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

TEST_P(SsmFrameIntegrityTest, PollRejectsWrongSender)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}, false));
    transport->QueueRead(BuildResponse(bytes::Bytes{42}));
    QueueNoFrames(*transport, 60);
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, false, GetParam());
    const auto result = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST_P(SsmFrameIntegrityTest, StartAcceptsExactlyOneProbeByteFromTcu)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}, false));
    // Hand-checked sum: 0x80 + 0xf0 + 0x18 + 2 + 0xe8 + 7 = 0x279.
    transport->QueueRead(bytes::Bytes{0x80, 0xf0, 0x18, 2, 0xe8, 7, 0x79});
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, false, GetParam());
    EXPECT_THAT(protocol.Start(cancellation), fastecu::testing::IsOk());
}

TEST_P(SsmFrameIntegrityTest, StartRejectsWrongSender)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}, false));
    transport->QueueRead(BuildResponse(bytes::Bytes{7}));
    QueueNoFrames(*transport, 110);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, false, GetParam());
    EXPECT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST_P(SsmFrameIntegrityTest, PollRejectsDeclaredSizeMismatch)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    // The checksum is valid, but the declared body asks for a missing byte.
    transport->QueueRead(bytes::Bytes{0x80, 0xf0, 0x10, 3, 0xe8, 42, 0x95});
    QueueNoFrames(*transport, 10);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, GetParam());
    const auto result = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(SsmLoggingProtocolTest, DirectPollCompletesFragmentedDeclaredFrame)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueRead(bytes::Bytes{0x01, 0x80});
    transport->QueueRead(bytes::Bytes{0xf0, 0x10, 2});
    transport->QueueNoFrame();
    transport->QueueRead(bytes::Bytes{0xe8, 42, 0x94});
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);
    const auto result = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

TEST(SsmLoggingProtocolTest, ReconnectDiscardsBufferedPollBeforeFreshProbe)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    auto replies = BuildResponse(bytes::Bytes{42});
    const auto buffered = BuildResponse(bytes::Bytes{99});
    replies.insert(replies.end(), buffered.begin(), buffered.end());
    transport->QueueRead(replies);
    auto probe = BuildResponse(bytes::Bytes{7});
    probe.back() ^= 1;
    transport->QueueRead(probe);
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    const auto first = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_TRUE(first->responded);
    EXPECT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(SsmLoggingProtocolTest, StopDiscardsBufferedPollBeforeNextRead)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    const auto request = BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00});
    transport->ExpectWrite(request);
    transport->ExpectWrite(request);
    auto replies = BuildResponse(bytes::Bytes{42});
    const auto buffered = BuildResponse(bytes::Bytes{99});
    replies.insert(replies.end(), buffered.begin(), buffered.end());
    transport->QueueRead(replies);
    transport->QueueRead(BuildResponse(bytes::Bytes{123}));
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    ASSERT_THAT(protocol.Poll(50ms, cancellation), fastecu::testing::IsOk());
    ASSERT_THAT(protocol.Stop(), fastecu::testing::IsOk());
    const auto next = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(next, fastecu::testing::IsOk());
    ASSERT_TRUE(next->responded);
    ASSERT_EQ(next->samples.size(), 1U);
    EXPECT_EQ(next->samples[0].raw_value, "123");
}

TEST(SsmLoggingProtocolTest, DirectPollRetainsCoalescedFramesReadAtDeadline)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    const auto request = BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00});
    transport->ExpectWrite(request);
    transport->ExpectWrite(request);
    auto replies = BuildResponse(bytes::Bytes{42});
    const auto buffered = BuildResponse(bytes::Bytes{99});
    replies.insert(replies.end(), buffered.begin(), buffered.end());
    transport->QueueRead(replies);
    QueueNoFrames(*transport, 60);
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    const auto first = protocol.Poll(2ms, cancellation);
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_TRUE(first->responded);
    ASSERT_EQ(first->samples.size(), 1U);
    EXPECT_EQ(first->samples[0].raw_value, "42");
    const auto next = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(next, fastecu::testing::IsOk());
    ASSERT_TRUE(next->responded);
    ASSERT_EQ(next->samples.size(), 1U);
    EXPECT_EQ(next->samples[0].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, DirectPollCapsEachReadToRemainingDeadline)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    // now() advances 3ms: the four remaining budgets are 11, 8, 5, 2ms.
    transport->ExpectReadTimeout(10ms);
    transport->QueueRead(bytes::Bytes{0x80});
    transport->ExpectReadTimeout(8ms);
    transport->QueueRead(bytes::Bytes{0xf0, 0x10});
    transport->ExpectReadTimeout(5ms);
    transport->QueueNoFrame();
    transport->ExpectReadTimeout(2ms);
    transport->QueueRead(bytes::Bytes{2, 0xe8, 42, 0x94});
    auto clock = fastecu::MakeAutoAdvancingClock(3ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {Channel()}, true, false);

    const auto result = protocol.Poll(14ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

INSTANTIATE_TEST_SUITE_P(DirectAndOpenPort, SsmFrameIntegrityTest, ::testing::Bool());
