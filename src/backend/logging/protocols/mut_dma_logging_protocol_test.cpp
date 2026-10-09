#include "src/backend/ports/testing/result_matchers.h"
#include <chrono>

#include <gtest/gtest.h>

#include "src/backend/protocol/testing/scripted_kline_transport.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_codec.h"
#include "src/backend/logging/protocols/portable_mut_dma_logging_protocol.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

namespace
{
using fastecu::logging::LoggingChannel;
using fastecu::logging::MutDmaLoggingProtocol;
using fastecu::logging::RawAssembly;
using mutdma::AlreadyInMode;
using mutdma::Channel;
using mutdma::ScriptedKlineTransport;
using namespace std::chrono_literals;

LoggingChannel MakeChannel()
{
    return LoggingChannel{
        .id = "mut.rpm",
        .address = 0x8000,
        .length = 2,
        .raw_assembly = RawAssembly::kUnsignedIntegerDecimal,
        .from_byte_expression = "x",
        .unit = "rpm",
        .decimal_precision = 0,
    };
}

void ScriptValidHandshake(ScriptedKlineTransport& transport)
{
    const std::vector<Channel> channels = {{0x8000, 2}};
    transport.ExpectWrite(mutdma::BuildSetupFrame(0xA0, 1));
    transport.QueueRead(mutdma::BuildCommandFrame(0xA5, bytes::Bytes{}, mutdma::kTrailerStd));
    transport.ExpectWrite(mutdma::BuildIdListFrame(0xA1, channels));
    transport.QueueRead(mutdma::BuildCommandFrame(0x05, bytes::Bytes{}, mutdma::kTrailerStd));
}

std::unique_ptr<MutDmaLoggingProtocol> MakeProtocol(std::unique_ptr<ScriptedKlineTransport> transport,
                                                    std::vector<LoggingChannel> channels = {MakeChannel()})
{
    return std::make_unique<MutDmaLoggingProtocol>(std::move(transport), std::make_unique<AlreadyInMode>(125000),
                                                   std::move(channels));
}
} // namespace

TEST(MutDmaLoggingProtocolTest, StartReachesStreamingOnValidHandshake)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    ScriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsOk());
    EXPECT_TRUE(script->ScriptConsumed());
    EXPECT_TRUE(script->Ok());
}

TEST(MutDmaLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->SetOpen(false);
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
}

TEST(MutDmaLoggingProtocolTest, StartFailurePinsBadResponseForInvalidHandshake)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->ExpectWrite(mutdma::BuildSetupFrame(0xA0, 1));
    transport->QueueRead(mutdma::BuildCommandFrame(0x00, bytes::Bytes{}, mutdma::kTrailerStd));
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(MutDmaLoggingProtocolTest, StartPropagatesDisconnectedSetBaudErrorKindAndDetail)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->QueueSetBaudError(fastecu::ErrorKind::kDisconnected, "sentinel core set-baud disconnect");
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "sentinel core set-baud disconnect"));
}

TEST(MutDmaLoggingProtocolTest, StartPropagatesInternalSetBaudErrorKindAndDetail)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->QueueSetBaudError(fastecu::ErrorKind::kInternal, "sentinel core set-baud internal");
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel core set-baud internal"));
}

TEST(MutDmaLoggingProtocolTest, StartPropagatesQueuedWriteErrorKindAndDetail)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->ExpectWrite(mutdma::BuildSetupFrame(0xA0, 1));
    transport->QueueWriteError(fastecu::ErrorKind::kDisconnected, "sentinel core setup write disconnect");
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "sentinel core setup write disconnect"));
}

TEST(MutDmaLoggingProtocolTest, StartPropagatesQueuedReadErrorKindAndDetail)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    transport->ExpectWrite(mutdma::BuildSetupFrame(0xA0, 1));
    transport->QueueError(fastecu::ErrorKind::kInternal, "sentinel core setup read internal");
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel core setup read internal"));
}

TEST(MutDmaLoggingProtocolTest, PollReturnsNoResponseBeforeStart)
{
    auto protocol = MakeProtocol(std::make_unique<ScriptedKlineTransport>());
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->Poll(20ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(MutDmaLoggingProtocolTest, PollReturnsTransportErrorWhenAdapterClosesMidSession)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    ScriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsOk());
    script->SetOpen(false);

    ASSERT_THAT(protocol->Poll(20ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
}

TEST(MutDmaLoggingProtocolTest, PollReturnsStableIdAndRawDecimalString)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    ScriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsOk());

    bytes::Bytes frame = {0x51, 0x12, 0x34};
    frame.push_back(mutdma::Sum8(frame));
    frame.push_back(mutdma::kTrailerStd);
    script->QueueRead(frame);

    const auto result = protocol->Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "mut.rpm");
    EXPECT_EQ(result->samples[0].raw_value, "4660");
}

TEST(MutDmaLoggingProtocolTest, StartPropagatesCancellation)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation(true);

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(MutDmaLoggingProtocol, PollRejectsShortChecksummedPayloadAndAcceptsNextCompleteReply)
{
    auto transport = std::make_unique<ScriptedKlineTransport>();
    scriptValidHandshake(*transport);
    transport->queueRead(bytes::Bytes{0x51, 0x12, 0x63, 0x0d});
    transport->queueRead(bytes::Bytes{0x51, 0x12, 0x34, 0x97, 0x0d});
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsOk());
    EXPECT_THAT(protocol->poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::BadResponse));
    const auto result = protocol->poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples.front().raw_value, "4660");
}
