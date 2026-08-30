#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/backend/protocol/cdbg_protocol_config.h"
#include "src/backend/protocol/testing/scripted_can_transport.h"
#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{
using fastecu::logging::CdbgLoggingProtocol;
using fastecu::logging::LoggingChannel;
using fastecu::logging::RawAssembly;
using MitsuColtCanCdbg::CdbgChannel;

LoggingChannel channel()
{
    return LoggingChannel{
        .id = "cdbg.load",
        .address = 0x804000,
        .length = 1,
        .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
        .from_byte_expression = "x",
        .unit = "%",
        .decimal_precision = 0,
    };
}

cdbg::CdbgProtocolConfig coltConfig()
{
    return *cdbg::make_colt_cdbg_protocol_config();
}

void scriptValidHandshake(cdbg::ScriptedCanTransport& transport, const cdbg::CdbgProtocolConfig& config)
{
    using namespace MitsuColtCanCdbg;
    const std::vector<CdbgChannel> channels = {{0x804000, 1}};

    transport.expectWrite(config.request_id(), buildInitFrame());
    transport.queueRead(config.reply_id(), test_bytes::bytesFromHex("0000000000000000"));
    transport.expectWrite(config.request_id(), buildSecuritySeedRequestFrame());
    transport.queueRead(config.reply_id(), test_bytes::bytesFromHex("0000000012345678"));
    transport.expectWrite(config.request_id(), buildSecurityKeyFrame(0x8C536B33));
    transport.queueRead(config.reply_id(), test_bytes::bytesFromHex("0000000100000000"));
    transport.expectWrite(config.request_id(), buildLogResetFrame(config.stream_instance()));
    transport.queueRead(config.reply_id(), test_bytes::bytesFromHex("0000000000000000"));

    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(batchChannelsIntoFrames(channels, frames));
    for (const auto& command : buildFrameInitFrames(config.stream_instance(), 0, frames.at(0)))
    {
        transport.expectWrite(config.request_id(), command);
        transport.queueRead(config.reply_id(), test_bytes::bytesFromHex("0000000000000000"));
    }
    transport.expectWrite(config.request_id(),
                          buildLogStartFrame(config.stream_instance(), 1, config.sampling_interval_ms()));
    transport.queueRead(config.reply_id(), test_bytes::bytesFromHex("0000000000000000"));
}

std::unique_ptr<CdbgLoggingProtocol> makeProtocol(std::unique_ptr<cdbg::ScriptedCanTransport> transport,
                                                  cdbg::CdbgProtocolConfig config,
                                                  std::vector<LoggingChannel> channels = {channel()})
{
    return std::make_unique<CdbgLoggingProtocol>(std::move(transport), std::move(channels), std::move(config));
}
} // namespace

TEST(CdbgLoggingProtocolTest, StartReachesStreamingOnValidHandshake)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    auto config = coltConfig();
    scriptValidHandshake(*transport, config);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport), std::move(config));
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_TRUE(result);
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(CdbgLoggingProtocolTest, StartUsesConfiguredCdbgWireValues)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    const std::vector<CdbgChannel> channels = {{0x804000, 1}};
    auto config = *cdbg::make_cdbg_protocol_config(0x620, 0x621, 3, 25);

    transport->expectWrite(0x620, MitsuColtCanCdbg::buildInitFrame());
    transport->queueRead(0x621, test_bytes::bytesFromHex("0000000000000000"));
    transport->expectWrite(0x620, MitsuColtCanCdbg::buildSecuritySeedRequestFrame());
    transport->queueRead(0x621, test_bytes::bytesFromHex("0000000012345678"));
    transport->expectWrite(0x620, MitsuColtCanCdbg::buildSecurityKeyFrame(0x8C536B33));
    transport->queueRead(0x621, test_bytes::bytesFromHex("0000000100000000"));
    transport->expectWrite(0x620, MitsuColtCanCdbg::buildLogResetFrame(3));
    transport->queueRead(0x621, test_bytes::bytesFromHex("0000000000000000"));

    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(MitsuColtCanCdbg::batchChannelsIntoFrames(channels, frames));
    for (const auto& command : MitsuColtCanCdbg::buildFrameInitFrames(3, 0, frames.at(0)))
    {
        transport->expectWrite(0x620, command);
        transport->queueRead(0x621, test_bytes::bytesFromHex("0000000000000000"));
    }
    transport->expectWrite(0x620, MitsuColtCanCdbg::buildLogStartFrame(3, 1, 25));
    transport->queueRead(0x621, test_bytes::bytesFromHex("0000000000000000"));

    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport), std::move(config));
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_TRUE(result);
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsInvalidConfigForEmptyChannels)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>(), coltConfig(), {});
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsBadResponseForMissingHandshakeReply)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->expectWrite(MitsuColtCanCdbg::kRequestCanId, MitsuColtCanCdbg::buildInitFrame());
    transport->queue_no_frame();
    auto protocol = makeProtocol(std::move(transport), coltConfig());
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
}

TEST(CdbgLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->setOpen(false);
    auto protocol = makeProtocol(std::move(transport), coltConfig());
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
}

TEST(CdbgLoggingProtocolTest, PollReturnsNoResponseBeforeStart)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>(), coltConfig());
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->poll(20, cancellation);

    ASSERT_TRUE(result);
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(CdbgLoggingProtocolTest, PollReturnsTransportErrorWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->setOpen(false);
    auto protocol = makeProtocol(std::move(transport), coltConfig());
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->poll(20, cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
}

TEST(CdbgLoggingProtocolTest, PollReturnsStableIdAndRawDecimalString)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    auto config = coltConfig();
    scriptValidHandshake(*transport, config);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport), std::move(config));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_TRUE(protocol->start(cancellation));
    script->queueRead(MitsuColtCanCdbg::kReplyCanId, test_bytes::bytesFromHex("002A000000000000"));

    const auto result = protocol->poll(50, cancellation);

    ASSERT_TRUE(result);
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "cdbg.load");
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

TEST(CdbgLoggingProtocolTest, PollReportsSilenceAfterStartWithoutCachedSamples)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    auto config = coltConfig();
    scriptValidHandshake(*transport, config);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport), std::move(config));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_TRUE(protocol->start(cancellation));
    script->queue_no_frame();

    const auto result = protocol->poll(50, cancellation);

    ASSERT_TRUE(result);
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(CdbgLoggingProtocolTest, StartPropagatesCancellation)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>(), coltConfig());
    fastecu::FakeCancellationToken cancellation(true);

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
}
