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

LoggingChannel channel(std::string id = "rpm", std::uint32_t address = 0x1000, std::size_t length = 1)
{
    return LoggingChannel{
        .id = std::move(id),
        .address = address,
        .length = length,
        .raw_assembly = RawAssembly::DecimalBytesConcatenated,
        .from_byte_expression = "x",
        .unit = "rpm",
        .decimal_precision = 0,
    };
}

bytes::Bytes buildRequest(bytes::ByteView payload, bool target_is_ecu = true)
{
    return SsmProtocol::addHeader(payload, 0xF0, target_is_ecu ? 0x10 : 0x18);
}

bytes::Bytes buildResponse(bytes::ByteView payload)
{
    bytes::Bytes message = {0x80, 0xf0, 0x10, static_cast<bytes::Byte>(payload.size() + 1), 0xe8};
    message.insert(message.end(), payload.begin(), payload.end());
    message.push_back(bytes::sum8(message));
    return message;
}

SsmLoggingProtocol make_protocol(fastecu::IClock& clock, std::unique_ptr<ScriptedSsmTransport> transport,
                                 std::vector<LoggingChannel> channels, bool target_is_ecu, bool openport)
{
    auto plan = fastecu::logging::make_ssm_read_plan(channels);
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return SsmLoggingProtocol(clock, std::move(transport), std::move(channels), std::move(*plan), target_is_ecu,
                              openport);
}
void queueNoFrames(ScriptedSsmTransport& transport, int count)
{
    for (int i = 0; i < count; ++i)
    {
        transport.queue_no_frame();
    }
}
} // namespace

TEST(SsmLoggingProtocolTest, StartPreservesHistoricalRequestVector)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->queueRead(buildResponse(bytes::Bytes{0}));
    transport->queue_no_frame();
    auto *script = transport.get();
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.start(cancellation), fastecu::testing::IsOk());
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(SsmLoggingProtocolTest, StartReturnsBadResponseForShortReply)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->queueRead(bytes::Bytes{});
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, true);

    ASSERT_THAT(protocol.start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::BadResponse));
}

TEST(SsmLoggingProtocolTest, StartReturnsBadResponseForNegativeReply)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    auto response = buildResponse(bytes::Bytes{0});
    response[4] = 0x7f;
    transport->queueRead(response);
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, true);

    ASSERT_THAT(protocol.start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::BadResponse));
}

TEST(SsmLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->setOpen(false);
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::Disconnected, "adapter disconnected"));
}

TEST(SsmLoggingProtocolTest, PreservesDecimalByteConcatenation)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x01}));
    transport->queueRead(buildResponse(bytes::Bytes{16, 16}));
    transport->queue_no_frame();
    auto *script = transport.get();
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel("rpm", 0x1000, 2)}, true, false);

    const auto result = protocol.poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "rpm");
    EXPECT_EQ(result->samples[0].raw_value, "1616");
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(SsmLoggingProtocolTest, PollPreservesChannelRequestOrder)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
    transport->queueRead(buildResponse(bytes::Bytes{42, 99}));
    transport->queue_no_frame();
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol =
        make_protocol(clock, std::move(transport), {channel("first", 0x1000), channel("second", 0x1003)}, true, false);

    const auto result = protocol.poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2U);
    EXPECT_EQ(result->samples[0].channel_id, "first");
    EXPECT_EQ(result->samples[0].raw_value, "42");
    EXPECT_EQ(result->samples[1].channel_id, "second");
    EXPECT_EQ(result->samples[1].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, PollMapsConsecutivePhysicalBytesIndependentOfMissingDisplaySlots)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
    transport->queueRead(buildResponse(bytes::Bytes{42, 99}));
    transport->queue_no_frame();
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol =
        make_protocol(clock, std::move(transport), {channel("first", 0x1000), channel("third", 0x1003)}, true, false);

    const auto result = protocol.poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2U);
    EXPECT_EQ(result->samples[0].raw_value, "42");
    EXPECT_EQ(result->samples[1].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, PollReturnsNoResponseOnDirectReadTimeout)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queue_no_frame();
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, true);

    const auto result = protocol.poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(SsmLoggingProtocolTest, HeaderResynchronizationRemainsDeadlineBounded)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queueRead(bytes::Bytes(64, 0x01));
    queueNoFrames(*transport, 8);
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    const auto result = protocol.poll(100ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::BadResponse));
}

TEST(SsmLoggingProtocolTest, CancellationDuringFramingReturnsCancelled)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queueRead(bytes::Bytes{0x80});
    transport->queueRead(bytes::Bytes{0xf0, 0x10});
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    cancellation.cancel_on_check(4);
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::Cancelled));
}

TEST(SsmLoggingProtocolTest, StartCancellationReturnsCancelledWithoutIo)
{
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation(true);
    auto protocol = make_protocol(clock, std::make_unique<ScriptedSsmTransport>(), {channel()}, true, false);

    ASSERT_THAT(protocol.start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::Cancelled));
}

TEST(SsmLoggingProtocolTest, PollPropagatesTypedWriteFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queue_write_error(fastecu::ErrorKind::Disconnected, "sentinel SSM write disconnect");
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::Disconnected, "sentinel SSM write disconnect"));
}

TEST(SsmLoggingProtocolTest, PollPropagatesTypedReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queue_error(fastecu::ErrorKind::Internal, "sentinel SSM read failure");
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::Internal, "sentinel SSM read failure"));
}

TEST(SsmLoggingProtocolTest, StartPropagatesTypedWriteFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->queue_write_error(fastecu::ErrorKind::Disconnected, "sentinel SSM start write disconnect");
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::Disconnected, "sentinel SSM start write disconnect"));
}

TEST(SsmLoggingProtocolTest, StartPropagatesTypedReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->queue_error(fastecu::ErrorKind::Internal, "sentinel SSM start read failure");
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::Internal, "sentinel SSM start read failure"));
}

TEST(SsmLoggingProtocolTest, PollPropagatesOpenPort2DirectReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queue_error(fastecu::ErrorKind::Disconnected, "sentinel OpenPort2 direct read failure");
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, true);

    ASSERT_THAT(
        protocol.poll(50ms, cancellation),
        fastecu::testing::IsErrWith(fastecu::ErrorKind::Disconnected, "sentinel OpenPort2 direct read failure"));
}

TEST(SsmLoggingProtocolTest, HeaderResynchronizationPropagatesReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    // Enough bytes to leave the initial accumulation loop, but not framed as
    // an SSM response (0x80 0xf0 0x10), so resynchronization must shift and
    // re-read -- and that re-read fails.
    transport->queueRead(bytes::Bytes{0x01, 0x02, 0x03});
    transport->queue_error(fastecu::ErrorKind::Internal, "sentinel SSM resync read failure");
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::Internal, "sentinel SSM resync read failure"));
}

TEST(SsmLoggingProtocolTest, FinalReadAfterHeaderMatchPropagatesReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    // Exactly a matching header: the accumulation loop stops (size >= 3), the
    // resynchronization loop is a no-op (header already matches), so the
    // trailing "read remaining payload" call executes -- and that read fails.
    transport->queueRead(bytes::Bytes{0x80, 0xf0, 0x10});
    transport->queue_error(fastecu::ErrorKind::Internal, "sentinel SSM final read failure");
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::Internal, "sentinel SSM final read failure"));
}

TEST(SsmLoggingProtocolTest, PollCancellationReturnsCancelledWithoutIo)
{
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation(true);
    auto protocol = make_protocol(clock, std::make_unique<ScriptedSsmTransport>(), {channel()}, true, false);

    ASSERT_THAT(protocol.poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::Cancelled));
}

TEST(SsmLoggingProtocolTest, PollFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->setOpen(false);
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, false);

    ASSERT_THAT(protocol.poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::Disconnected, "adapter disconnected"));
}

TEST(SsmLoggingProtocolTest, ChecksumValidShortReplyDoesNotPublishPartialBatch)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{0xa8, 1, 0, 0x10, 0, 0, 0x10, 1, 0, 0x10, 3}));
    transport->queueRead(buildResponse(bytes::Bytes{7, 8}));
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol =
        make_protocol(clock, std::move(transport), {channel("wide", 0x1000, 2), channel("single", 0x1003)}, true, true);
    EXPECT_THAT(protocol.poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::BadResponse));
}

TEST(SsmLoggingProtocolTest, StopSucceeds)
{
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    auto protocol = make_protocol(clock, std::make_unique<ScriptedSsmTransport>(), {channel()}, true, false);

    EXPECT_THAT(protocol.stop(), fastecu::testing::IsOk());
}

TEST(SsmLoggingProtocolTest, RequestsEachPhysicalByteForAMultiByteValue)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    // Independent literal two-address request and response.
    transport->expectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x08, 0xa8, 0x01, 0, 0, 0x10, 0, 0, 0x11, 0x52});
    transport->queueRead(bytes::Bytes{0x80, 0xf0, 0x10, 0x03, 0xe8, 1, 2, 0x6e});
    auto clock = fastecu::make_auto_advancing_clock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol = make_protocol(clock, std::move(transport), {channel("wide", 0x10, 2)}, true, true);
    const auto result = protocol.poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].raw_value, "12");
}

TEST(SsmLoggingProtocolTest, StartRejectsInvalidChecksumWrongTargetAndExtraData)
{
    const std::vector<bytes::Bytes> replies{{0x80, 0xf0, 0x10, 2, 0xe8, 0, 0x6b},
                                            {0x80, 0xf0, 0x18, 2, 0xe8, 0, 0x72},
                                            {0x80, 0xf0, 0x10, 3, 0xe8, 0, 0, 0x6b}};
    for (const auto& reply : replies)
    {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->expectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 5, 0xa8, 0, 0, 0, 7, 0x34});
        transport->queueRead(reply);
        auto clock = fastecu::make_auto_advancing_clock(10ms);
        fastecu::FakeCancellationToken cancellation;
        auto protocol = make_protocol(clock, std::move(transport), {channel()}, true, true);
        EXPECT_THAT(protocol.start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::BadResponse));
    }
}
