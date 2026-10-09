#include "src/backend/ports/testing/result_matchers.h"
#include <chrono>

#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/backend/protocol/testing/scripted_can_transport.h"
#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{
using fastecu::logging::CdbgLoggingProtocol;
using fastecu::logging::LoggingChannel;
using fastecu::logging::RawAssembly;
using mitsu_colt_can_cdbg::CdbgChannel;
using namespace std::chrono_literals;

LoggingChannel channel()
{
    return LoggingChannel{
        .id = "cdbg.load",
        .address = 0x804000,
        .length = 1,
        .raw_assembly = RawAssembly::kUnsignedIntegerDecimal,
        .from_byte_expression = "x",
        .unit = "%",
        .decimal_precision = 0,
    };
}

void scriptValidHandshake(cdbg::ScriptedCanTransport& transport)
{
    using namespace mitsu_colt_can_cdbg;
    const std::vector<CdbgChannel> channels = {{0x804000, 1}};

    transport.expectWrite(kRequestCanId, BuildInitFrame());
    transport.queueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
    transport.expectWrite(kRequestCanId, BuildSecuritySeedRequestFrame());
    transport.queueRead(kReplyCanId, test_bytes::BytesFromHex("0000000012345678"));
    transport.expectWrite(kRequestCanId, BuildSecurityKeyFrame(0x8C536B33));
    transport.queueRead(kReplyCanId, test_bytes::BytesFromHex("0000000100000000"));
    transport.expectWrite(kRequestCanId, BuildLogResetFrame(0));
    transport.queueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));

    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(BatchChannelsIntoFrames(channels, frames));
    for (const auto& command : BuildFrameInitFrames(0, 0, frames.at(0)))
    {
        transport.expectWrite(kRequestCanId, command);
        transport.queueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
    }
    transport.expectWrite(kRequestCanId, BuildLogStartFrame(0, 1, 10));
    transport.queueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
}

std::unique_ptr<CdbgLoggingProtocol> makeProtocol(std::unique_ptr<cdbg::ScriptedCanTransport> transport,
                                                  std::vector<LoggingChannel> channels = {channel()})
{
    return std::make_unique<CdbgLoggingProtocol>(std::move(transport), std::move(channels));
}
} // namespace

TEST(CdbgLoggingProtocolTest, StartReachesStreamingOnValidHandshake)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsOk());
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsInvalidConfigForEmptyChannels)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>(), {});
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsBadResponseForMissingHandshakeReply)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->expectWrite(mitsu_colt_can_cdbg::kRequestCanId, mitsu_colt_can_cdbg::BuildInitFrame());
    transport->queue_no_frame();
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(CdbgLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->setOpen(false);
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
}

TEST(CdbgLoggingProtocolTest, PollReturnsNoResponseBeforeStart)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>());
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->poll(20ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(CdbgLoggingProtocolTest, PollReturnsTransportErrorWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->setOpen(false);
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->poll(20ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
}

TEST(CdbgLoggingProtocolTest, PollReturnsStableIdAndRawDecimalString)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsOk());
    script->queueRead(mitsu_colt_can_cdbg::kReplyCanId, test_bytes::BytesFromHex("002A000000000000"));

    const auto result = protocol->poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "cdbg.load");
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

TEST(CdbgLoggingProtocolTest, PollReportsSilenceAfterStartWithoutCachedSamples)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsOk());
    script->queue_no_frame();

    const auto result = protocol->poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(CdbgLoggingProtocolTest, StartPropagatesCancellation)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>());
    fastecu::FakeCancellationToken cancellation(true);

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}
