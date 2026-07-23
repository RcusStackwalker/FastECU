#include <gtest/gtest.h>

#include "scripted_kline_transport.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/backend/logging/protocols/portable_mut_dma_logging_protocol.h"

namespace
{
using fastecu::logging::LoggingChannel;
using fastecu::logging::MutDmaLoggingProtocol;
using fastecu::logging::RawAssembly;
using mutdma::AlreadyInMode;
using mutdma::Channel;
using mutdma::ScriptedKlineTransport;

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
        .id = "mut.rpm",
        .address = 0x8000,
        .length = 2,
        .raw_assembly = RawAssembly::UnsignedIntegerDecimal,
        .from_byte_expression = "x",
        .unit = "rpm",
        .decimal_precision = 0,
    };
}

void scriptValidHandshake(ScriptedKlineTransport& transport)
{
    const std::vector<Channel> channels = {{0x8000, 2}};
    transport.expectWrite(mutdma::buildSetupFrame(0xA0, 1));
    transport.queueRead(mutdma::buildCommandFrame(
        0xA5, bytes::Bytes{}, mutdma::TRAILER_STD));
    transport.expectWrite(mutdma::buildIdListFrame(0xA1, channels));
    transport.queueRead(mutdma::buildCommandFrame(
        0x05, bytes::Bytes{}, mutdma::TRAILER_STD));
}

std::unique_ptr<MutDmaLoggingProtocol> makeProtocol(
    std::unique_ptr<ScriptedKlineTransport> transport,
    std::vector<LoggingChannel> channels = {channel()})
{
    return std::make_unique<MutDmaLoggingProtocol>(
        std::move(transport), std::make_unique<AlreadyInMode>(125000),
        std::move(channels));
}
} // namespace

TEST(MutDmaLoggingProtocolTest, StartReachesStreamingOnValidHandshake)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_TRUE(result);
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(MutDmaLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->setOpen(false);
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
}

TEST(MutDmaLoggingProtocolTest, StartFailurePinsBadResponseForInvalidHandshake)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->expectWrite(mutdma::buildSetupFrame(0xA0, 1));
    transport->queueRead(mutdma::buildCommandFrame(
        0x00, bytes::Bytes{}, mutdma::TRAILER_STD));
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
}

TEST(MutDmaLoggingProtocolTest,
     StartPropagatesDisconnectedSetBaudErrorKindAndDetail)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->queue_set_baud_error(fastecu::ErrorKind::Disconnected,
                                    "sentinel core set-baud disconnect");
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "sentinel core set-baud disconnect");
}

TEST(MutDmaLoggingProtocolTest,
     StartPropagatesInternalSetBaudErrorKindAndDetail)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->queue_set_baud_error(fastecu::ErrorKind::Internal,
                                    "sentinel core set-baud internal");
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_EQ(result.error().detail, "sentinel core set-baud internal");
}

TEST(MutDmaLoggingProtocolTest, StartPropagatesQueuedWriteErrorKindAndDetail)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->expectWrite(mutdma::buildSetupFrame(0xA0, 1));
    transport->queue_write_error(fastecu::ErrorKind::Disconnected,
                                 "sentinel core setup write disconnect");
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "sentinel core setup write disconnect");
}

TEST(MutDmaLoggingProtocolTest, StartPropagatesQueuedReadErrorKindAndDetail)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->expectWrite(mutdma::buildSetupFrame(0xA0, 1));
    transport->queue_error(fastecu::ErrorKind::Internal,
                           "sentinel core setup read internal");
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_EQ(result.error().detail, "sentinel core setup read internal");
}

TEST(MutDmaLoggingProtocolTest, PollReturnsNoResponseBeforeStart)
{
    auto protocol = makeProtocol(std::make_unique<ScriptedKlineTransport>());
    NeverCancelled cancellation;

    const auto result = protocol->poll(20, cancellation);

    ASSERT_TRUE(result);
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(MutDmaLoggingProtocolTest, PollReturnsTransportErrorWhenAdapterClosesMidSession)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;
    ASSERT_TRUE(protocol->start(cancellation));
    script->setOpen(false);

    const auto result = protocol->poll(20, cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
}

TEST(MutDmaLoggingProtocolTest, PollReturnsStableIdAndRawDecimalString)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    NeverCancelled cancellation;
    ASSERT_TRUE(protocol->start(cancellation));

    bytes::Bytes frame = {0x51, 0x12, 0x34};
    frame.push_back(mutdma::sum8(frame));
    frame.push_back(mutdma::TRAILER_STD);
    script->queueRead(frame);

    const auto result = protocol->poll(50, cancellation);

    ASSERT_TRUE(result);
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1u);
    EXPECT_EQ(result->samples[0].channel_id, "mut.rpm");
    EXPECT_EQ(result->samples[0].raw_value, "4660");
}

TEST(MutDmaLoggingProtocolTest, StartPropagatesCancellation)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    auto protocol = makeProtocol(std::move(transport));
    Cancelled cancellation;

    const auto result = protocol->start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
}
