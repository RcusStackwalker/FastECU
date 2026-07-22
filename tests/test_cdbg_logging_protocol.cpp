#include <gtest/gtest.h>

#include "byte_test_utils.h"
#include "scripted_can_transport.h"
#include "src/backend/logging/protocols/portable_cdbg_logging_protocol.h"

namespace
{
using fastecu::logging::CdbgLoggingProtocol;
using fastecu::logging::LoggingChannel;
using fastecu::logging::RawAssembly;
using MitsuColtCanCdbg::CdbgChannel;

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

void scriptValidHandshake(cdbg::ScriptedCanTransport& transport)
{
    using namespace MitsuColtCanCdbg;
    const std::vector<CdbgChannel> channels = {{0x804000, 1}};

    transport.expectWrite(kRequestCanId, buildInitFrame());
    transport.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
    transport.expectWrite(kRequestCanId, buildSecuritySeedRequestFrame());
    transport.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000012345678"));
    transport.expectWrite(kRequestCanId, buildSecurityKeyFrame(0x8C536B33));
    transport.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000100000000"));
    transport.expectWrite(kRequestCanId, buildLogResetFrame(0));
    transport.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));

    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(batchChannelsIntoFrames(channels, frames));
    for (const auto& command : buildFrameInitFrames(0, 0, frames.at(0)))
    {
        transport.expectWrite(kRequestCanId, command);
        transport.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
    }
    transport.expectWrite(kRequestCanId, buildLogStartFrame(0, 1, 10));
    transport.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
}

std::unique_ptr<CdbgLoggingProtocol> makeProtocol(
    std::unique_ptr<cdbg::ScriptedCanTransport> transport,
    std::vector<LoggingChannel> channels = {channel()})
{
    return std::make_unique<CdbgLoggingProtocol>(std::move(transport),
                                                 std::move(channels));
}
} // namespace

TEST(CdbgLoggingProtocolTest, StartReachesStreamingOnValidHandshake)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_TRUE(result);
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsInvalidConfigForEmptyChannels)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>(), {});
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsBadResponseForMissingHandshakeReply)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->expectWrite(MitsuColtCanCdbg::kRequestCanId,
                           MitsuColtCanCdbg::buildInitFrame());
    transport->queue_no_frame();
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
}

TEST(CdbgLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->setOpen(false);
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
}

TEST(CdbgLoggingProtocolTest, PollReturnsNoResponseBeforeStart)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>());
    NeverCancelled cancellation;

    const auto result = protocol->poll(20, cancellation);

    ASSERT_TRUE(result);
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(CdbgLoggingProtocolTest, PollReturnsTransportErrorWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->setOpen(false);
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->poll(20, cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
}

TEST(CdbgLoggingProtocolTest, PollReturnsStableIdAndRawDecimalString)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;
    ASSERT_TRUE(protocol->start(cancellation));
    script->queueRead(MitsuColtCanCdbg::kReplyCanId,
                      test_bytes::bytesFromHex("002A000000000000"));

    const auto result = protocol->poll(50, cancellation);

    ASSERT_TRUE(result);
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1u);
    EXPECT_EQ(result->samples[0].channel_id, "cdbg.load");
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

TEST(CdbgLoggingProtocolTest, StartPropagatesCancellation)
{
    auto protocol = makeProtocol(std::make_unique<cdbg::ScriptedCanTransport>());
    Cancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
}
