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

LoggingChannel Channel()
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

void ScriptValidHandshake(cdbg::ScriptedCanTransport& transport)
{
    using namespace mitsu_colt_can_cdbg;
    const std::vector<CdbgChannel> channels = {{0x804000, 1}};

    transport.ExpectWrite(kRequestCanId, BuildInitFrame());
    transport.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
    transport.ExpectWrite(kRequestCanId, BuildSecuritySeedRequestFrame());
    transport.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000012345678"));
    transport.ExpectWrite(kRequestCanId, BuildSecurityKeyFrame(0x8C536B33));
    transport.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000100000000"));
    transport.ExpectWrite(kRequestCanId, BuildLogResetFrame(0));
    transport.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));

    std::vector<std::vector<CdbgChannel>> frames;
    ASSERT_TRUE(BatchChannelsIntoFrames(channels, frames));
    for (const auto& command : BuildFrameInitFrames(0, 0, frames.at(0)))
    {
        transport.ExpectWrite(kRequestCanId, command);
        transport.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
    }
    transport.ExpectWrite(kRequestCanId, BuildLogStartFrame(0, 1, 10));
    transport.QueueRead(kReplyCanId, test_bytes::BytesFromHex("0000000000000000"));
}

std::unique_ptr<CdbgLoggingProtocol> MakeProtocol(std::unique_ptr<cdbg::ScriptedCanTransport> transport,
                                                  std::vector<LoggingChannel> channels = {Channel()})
{
    return std::make_unique<CdbgLoggingProtocol>(std::move(transport), std::move(channels));
}
} // namespace

TEST(CdbgLoggingProtocolTest, StartReachesStreamingOnValidHandshake)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    ScriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsOk());
    EXPECT_TRUE(script->ScriptConsumed());
    EXPECT_TRUE(script->Ok());
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsInvalidConfigForEmptyChannels)
{
    auto protocol = MakeProtocol(std::make_unique<cdbg::ScriptedCanTransport>(), {});
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
}

TEST(CdbgLoggingProtocolTest, StartFailurePinsBadResponseForMissingHandshakeReply)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->ExpectWrite(mitsu_colt_can_cdbg::kRequestCanId, mitsu_colt_can_cdbg::BuildInitFrame());
    transport->QueueNoFrame();
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(CdbgLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->SetOpen(false);
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
}

TEST(CdbgLoggingProtocolTest, PollReturnsNoResponseBeforeStart)
{
    auto protocol = MakeProtocol(std::make_unique<cdbg::ScriptedCanTransport>());
    fastecu::FakeCancellationToken cancellation;

    const auto result = protocol->Poll(20ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(CdbgLoggingProtocolTest, PollReturnsTransportErrorWhenAdapterIsClosed)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    transport->SetOpen(false);
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;

    ASSERT_THAT(protocol->Poll(20ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
}

TEST(CdbgLoggingProtocolTest, PollReturnsStableIdAndRawDecimalString)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    ScriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsOk());
    script->QueueRead(mitsu_colt_can_cdbg::kReplyCanId, test_bytes::BytesFromHex("002A000000000000"));

    const auto result = protocol->Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "cdbg.load");
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

TEST(CdbgLoggingProtocolTest, PollReportsSilenceAfterStartWithoutCachedSamples)
{
    auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
    ScriptValidHandshake(*transport);
    auto *script = transport.get();
    auto protocol = MakeProtocol(std::move(transport));
    fastecu::FakeCancellationToken cancellation;
    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsOk());
    script->QueueNoFrame();

    const auto result = protocol->Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(CdbgLoggingProtocolTest, StartPropagatesCancellation)
{
    auto protocol = MakeProtocol(std::make_unique<cdbg::ScriptedCanTransport>());
    fastecu::FakeCancellationToken cancellation(true);

    ASSERT_THAT(protocol->Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}
