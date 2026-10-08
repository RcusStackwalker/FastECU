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
using MitsuColtCanCdbg::CdbgChannel;
using namespace std::chrono_literals;

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

void scriptValidHandshake(cdbg::ScriptedCanTransport& transport,
                          const std::vector<CdbgChannel>& channels = {{0x804000, 1}})
{
    using namespace MitsuColtCanCdbg;

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
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        for (const auto& command : buildFrameInitFrames(0, static_cast<bytes::Byte>(index), frames[index]))
        {
            transport.expectWrite(kRequestCanId, command);
            transport.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
        }
    }
    transport.expectWrite(kRequestCanId, buildLogStartFrame(0, static_cast<bytes::Byte>(frames.size()), 10));
    transport.queueRead(kReplyCanId, test_bytes::bytesFromHex("0000000000000000"));
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

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::InvalidConfig));
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsBadResponseForMissingHandshakeReply)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->expectWrite(MitsuColtCanCdbg::kRequestCanId, MitsuColtCanCdbg::buildInitFrame());
    transport->queue_no_frame();
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::BadResponse));
}

TEST(CdbgLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->setOpen(false);
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::Disconnected));
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

    ASSERT_THAT(protocol->poll(20ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::Disconnected));
}

TEST(CdbgLoggingProtocolTest, PollReturnsStableIdAndRawDecimalString)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    scriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsOk());
    script->queueRead(MitsuColtCanCdbg::kReplyCanId, test_bytes::bytesFromHex("002A000000000000"));

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

    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::Cancelled));
}

TEST(CdbgLoggingProtocolTest, OutOfOrderFramesPublishOnlyTheirMappedMeasurements)
{
    auto first = channel();
    first.id = "first";
    first.length = 4;
    auto second = first;
    second.id = "second";
    second.address += 4;
    auto last = first;
    last.id = "last";
    last.address += 8;
    last.length = 2;
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    scriptValidHandshake(*transport, {{0x804000, 4}, {0x804004, 4}, {0x804008, 2}});
    auto *script = transport.get();
    auto protocol = makeProtocol(std::move(transport), {first, second, last});
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->start(cancellation), fastecu::testing::IsOk());
    script->queueRead(MitsuColtCanCdbg::kReplyCanId, bytes::Bytes{1, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0});
    const auto later = protocol->poll(50ms, cancellation);
    ASSERT_THAT(later, fastecu::testing::IsOk());
    ASSERT_EQ(later->samples.size(), 2U);
    EXPECT_EQ(later->samples[0].channel_id, "second");
    EXPECT_EQ(later->samples[0].raw_value, "287454020");
    EXPECT_EQ(later->samples[1].channel_id, "last");
    EXPECT_EQ(later->samples[1].raw_value, "21862");
    script->queueRead(MitsuColtCanCdbg::kReplyCanId, bytes::Bytes{0, 0xaa, 0xbb, 0xcc, 0xdd, 0, 0, 0});
    const auto earlier = protocol->poll(50ms, cancellation);
    ASSERT_THAT(earlier, fastecu::testing::IsOk());
    ASSERT_EQ(earlier->samples.size(), 1U);
    EXPECT_EQ(earlier->samples[0].channel_id, "first");
    EXPECT_EQ(earlier->samples[0].raw_value, "2864434397");
}
