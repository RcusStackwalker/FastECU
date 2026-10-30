#include <array>
#include "src/backend/ports/testing/result_matchers.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include "src/backend/logging/logger_definition_parser.h"
#include "src/backend/logging/logging_channel_preparation.h"
#include "src/backend/logging/logging_conversion.h"
#include "src/backend/logging/logging_session.h"
#include <memory>
#include <utility>
#include <vector>

#include "src/backend/protocol/testing/scripted_ssm_transport.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"

using ::testing::IsEmpty;

namespace
{
using namespace std::chrono_literals;
using fastecu::logging::LoggingChannel;
using fastecu::logging::SsmLoggingProtocol;

LoggingChannel Channel(std::string id = "rpm", std::uint32_t address = 0x1000, std::size_t length = 1)
{
    return LoggingChannel{
        .id = std::move(id),
        .address = address,
        .length = length,
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

fastecu::Result<SsmLoggingProtocol> MakeProtocol(fastecu::IClock& clock,
                                                 std::unique_ptr<ScriptedSsmTransport> transport,
                                                 std::vector<LoggingChannel> channels, bool target_is_ecu,
                                                 bool openport)
{
    auto plan = fastecu::logging::MakeSsmReadPlan(channels);
    if (!plan.has_value())
    {
        return std::unexpected(plan.error());
    }
    return SsmLoggingProtocol(clock, std::move(transport), std::move(channels), std::move(*plan), target_is_ecu,
                              openport);
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
    transport->QueueRead(BuildResponse(bytes::Bytes{0}));
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    ASSERT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(SsmLoggingProtocolTest, StartReturnsBadResponseForNegativeReply)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    auto response = BuildResponse(bytes::Bytes{0});
    response[4] = 0x7f;
    response.back() = bytes::Sum8(bytes::ByteView(response).first(response.size() - 1));
    transport->QueueRead(response);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    ASSERT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(SsmLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->SetOpen(false);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    ASSERT_THAT(protocol.Start(cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "adapter disconnected"));
}

TEST(SsmLoggingProtocolTest, DecodesBigEndianNumericBytes)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x01}));
    transport->QueueRead(BuildResponse(bytes::Bytes{1, 2}));
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel("rpm", 0x1000, 2)}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].channel_id, "rpm");
    EXPECT_EQ(result->samples[0].raw_value, "258");
    EXPECT_TRUE(script->ScriptConsumed());
    EXPECT_TRUE(script->Ok());
}

TEST(SsmLoggingProtocolTest, PollPreservesChannelRequestOrder)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
    transport->QueueRead(BuildResponse(bytes::Bytes{42, 99}));
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result =
        MakeProtocol(clock, std::move(transport), {Channel("first", 0x1000), Channel("second", 0x1003)}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2U);
    EXPECT_EQ(result->samples[0].channel_id, "first");
    EXPECT_EQ(result->samples[0].raw_value, "42");
    EXPECT_EQ(result->samples[1].channel_id, "second");
    EXPECT_EQ(result->samples[1].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, MapsUnequalWidthChannelsThroughPackedResponseBytes)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0, 0x10, 0, 0, 0x10, 1, 0, 0x20, 0}));
    transport->QueueRead(BuildResponse(bytes::Bytes{1, 2, 99}));
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result =
        MakeProtocol(clock, std::move(transport), {Channel("first", 0x1000, 2), Channel("second", 0x2000)}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto result = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2U);
    EXPECT_EQ(result->samples[0].raw_value, "258");
    EXPECT_EQ(result->samples[1].raw_value, "99");
    EXPECT_TRUE(script->Ok());
}

TEST(SsmLoggingProtocolTest, PollHonorsNonmonotonicResponsePositions)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(
        BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03, 0x00, 0x10, 0x04}));
    transport->QueueRead(BuildResponse(bytes::Bytes{42, 77, 99}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    SsmLoggingProtocol protocol(
        clock, std::move(transport), {Channel("first", 0x1000), Channel("third", 0x1003)},
        fastecu::logging::SsmReadPlan{.addresses = {0x1000, 0x1003, 0x1004}, .response_positions = {{2}, {0}}}, true,
        false);

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2U);
    EXPECT_EQ(result->samples[0].raw_value, "99");
    EXPECT_EQ(result->samples[1].raw_value, "42");
}

TEST(SsmLoggingProtocolTest, PollReturnsNoResponseOnDirectReadTimeout)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueNoFrame();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    const auto result = protocol.Poll(50ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->responded);
    EXPECT_THAT(result->samples, IsEmpty());
}

TEST(SsmLoggingProtocolTest, HeaderResynchronizationRemainsDeadlineBounded)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueRead(bytes::Bytes(64, 0x01));
    QueueNoFrames(*transport, 8);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    const auto result = protocol.Poll(100ms, cancellation);

    ASSERT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(SsmLoggingProtocolTest, CancellationDuringFramingReturnsCancelled)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueRead(bytes::Bytes{0x80});
    transport->QueueRead(bytes::Bytes{0xf0, 0x10});
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    cancellation.CancelOnCheck(4);
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    ASSERT_THAT(protocol.Poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(SsmLoggingProtocolTest, StartCancellationReturnsCancelledWithoutIo)
{
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation(true);
    auto protocol_result = MakeProtocol(clock, std::make_unique<ScriptedSsmTransport>(), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    ASSERT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(SsmLoggingProtocolTest, PollPropagatesTypedWriteFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->QueueWriteError(fastecu::ErrorKind::kDisconnected, "sentinel SSM write disconnect");
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    ASSERT_THAT(protocol.Poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kInternal, "sentinel SSM final read failure"));
}

TEST(SsmLoggingProtocolTest, PollCancellationReturnsCancelledWithoutIo)
{
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation(true);
    auto protocol_result = MakeProtocol(clock, std::make_unique<ScriptedSsmTransport>(), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    ASSERT_THAT(protocol.Poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(SsmLoggingProtocolTest, PollFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->SetOpen(false);
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    ASSERT_THAT(protocol.Poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kDisconnected, "adapter disconnected"));
}

TEST(SsmLoggingProtocolTest, ChecksumValidShortReplyDoesNotPublishPartialBatch)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xa8, 1, 0, 0x10, 0, 0, 0x10, 1, 0, 0x10, 3}));
    transport->QueueRead(BuildResponse(bytes::Bytes{7, 8}));
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result =
        MakeProtocol(clock, std::move(transport), {Channel("wide", 0x1000, 2), Channel("single", 0x1003)}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    EXPECT_THAT(protocol.Poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(SsmLoggingProtocolTest, RejectsExtraPayloadWithoutSamples)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0, 0x10, 0}));
    transport->QueueRead(BuildResponse(bytes::Bytes{42, 99}));
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto result = protocol.Poll(50ms, cancellation);
    EXPECT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST(SsmLoggingProtocolTest, RejectsIncompleteNumericPayloadWithoutSamples)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x01}));
    transport->QueueRead(BuildResponse(bytes::Bytes{1}));
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel("rpm", 0x1000, 2)}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto result = protocol.Poll(50ms, cancellation);
    EXPECT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

struct NumericCase
{
    const char *storage;
    const char *endian;
    bytes::Bytes payload;
    double expected;
};
class SsmNumericDefinitionTest : public ::testing::TestWithParam<NumericCase>
{
};
TEST_P(SsmNumericDefinitionTest, DecodesBeforeCheckedScaling)
{
    const auto& item = GetParam();
    const auto xml = std::format(R"(<logger><protocols><protocol id="SSM"><parameters>
      <parameter id="p"><address length="{}">0x1000</address><conversions>
      <conversion {} {} expr="x*2" format="0"/>
      <conversion storagetype="float" endian="little" expr="x/2" format="0"/>
      </conversions></parameter></parameters></protocol></protocols></logger>)",
                                 item.payload.size(), item.storage, item.endian);
    const auto definition = fastecu::logging::ParseLoggerDefinition(
        {reinterpret_cast<const bytes::Byte *>(xml.data()), xml.size()}, "numeric fixture");
    ASSERT_THAT(definition, fastecu::testing::IsOk());
    ASSERT_EQ(definition->parameters.front().conversions.size(), 2U);
    EXPECT_EQ(definition->parameters.front().conversions[1].storage_type, "float");
    EXPECT_EQ(definition->parameters.front().conversions[1].endian, "little");
    const auto channel = fastecu::logging::PrepareLoggingChannel(definition->parameters.front(),
                                                                 fastecu::logging::LoggingProtocolId::kSsm);
    ASSERT_THAT(channel, fastecu::testing::IsOk());
    auto transport = std::make_unique<ScriptedSsmTransport>();
    const auto request = item.payload.size() == 1 ? bytes::Bytes{0xA8, 0x01, 0, 0x10, 0}
                         : item.payload.size() == 2
                             ? bytes::Bytes{0xA8, 0x01, 0, 0x10, 0, 0, 0x10, 1}
                             : bytes::Bytes{0xA8, 0x01, 0, 0x10, 0, 0, 0x10, 1, 0, 0x10, 2, 0, 0x10, 3};
    transport->ExpectWrite(BuildRequest(request));
    transport->QueueRead(BuildResponse(item.payload));
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {*channel}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto poll = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(poll, fastecu::testing::IsOk());
    ASSERT_TRUE(poll->responded);
    ASSERT_EQ(poll->samples.size(), 1U);
    const auto session =
        fastecu::logging::MakeLoggingSession(fastecu::logging::LoggingProtocolId::kSsm, {*channel}, {50ms, 3, 2, 0});
    ASSERT_THAT(session, fastecu::testing::IsOk());
    const auto converted = fastecu::logging::ConvertSample(*session, poll->samples.front());
    ASSERT_THAT(converted, fastecu::testing::IsOk());
    EXPECT_EQ(converted->numeric_value, item.expected * 2);
    EXPECT_TRUE(script->Ok());
    EXPECT_TRUE(script->ScriptConsumed());
}
INSTANTIATE_TEST_SUITE_P(
    LiteralBytes, SsmNumericDefinitionTest,
    ::testing::Values(NumericCase{"", "", {1, 2}, 258}, NumericCase{"", "", {0xff}, 255},
                      NumericCase{"storagetype=\"uint8\"", "", {0xff, 0xff, 0xff, 0xff}, 4294967295.0},
                      NumericCase{"storagetype=\"int8\"", "", {0xfe}, -2},
                      NumericCase{"storagetype=\"INT32\"", "endian=\"BIG\"", {0x80}, -128},
                      NumericCase{"storagetype=\"int16\"", "", {0xff, 0xfe}, -2},
                      NumericCase{"storagetype=\"int32\"", "", {0xff, 0xff, 0xff, 0xfe}, -2},
                      NumericCase{"storagetype=\"uint16\"", "endian=\"little\"", {1, 2}, 513},
                      NumericCase{"storagetype=\"int16\"", "endian=\"little\"", {0xfe, 0xff}, -2},
                      NumericCase{"storagetype=\"float\"", "", {0x3f, 0xc0, 0, 0}, 1.5},
                      NumericCase{"storagetype=\"float\"", "", {0x3f, 0x80, 0, 1}, 1.00000011920928955078125},
                      NumericCase{"storagetype=\"float\"", "endian=\"little\"", {0, 0, 0xc0, 0x3f}, 1.5}),
    (
        [](const auto& param_info)
        {
            static constexpr std::array kNames{"DefaultTwoBytes",
                                               "DefaultOneByte",
                                               "Uint8LabelStillUsesFullPayload",
                                               "Int8Negative",
                                               "UppercaseStorageAndEndian",
                                               "Int16Negative",
                                               "Int32Negative",
                                               "Uint16LittleEndian",
                                               "Int16LittleEndian",
                                               "FloatBigEndian",
                                               "FloatMantissaLowBit",
                                               "FloatLittleEndian"};
            return kNames[param_info.index];
        }));

TEST(SsmLoggingProtocolTest, StopSucceeds)
{
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    auto protocol_result = MakeProtocol(clock, std::make_unique<ScriptedSsmTransport>(), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    EXPECT_THAT(protocol.Stop(), fastecu::testing::IsOk());
}

TEST(SsmLoggingProtocolTest, RequestsEachPhysicalByteForAMultiByteValue)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    // Independent literal two-address request and response.
    transport->ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x08, 0xa8, 0x01, 0, 0, 0x10, 0, 0, 0x11, 0x52});
    transport->QueueRead(bytes::Bytes{0x80, 0xf0, 0x10, 0x03, 0xe8, 1, 2, 0x6e});
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel("wide", 0x10, 2)}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto result = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].raw_value, "258");
}

TEST(SsmLoggingProtocolTest, StartRejectsInvalidChecksumWrongTargetAndExtraData)
{
    const std::vector<bytes::Bytes> replies{{0x80, 0xf0, 0x10, 2, 0xe8, 0, 0x6b},
                                            {0x80, 0xf0, 0x18, 2, 0xe8, 0, 0x72},
                                            {0x80, 0xf0, 0x10, 3, 0xe8, 0, 0, 0x6b}};
    for (const auto& reply : replies)
    {
        SCOPED_TRACE(::testing::PrintToString(reply));
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 5, 0xa8, 0, 0, 0, 7, 0x34});
        transport->QueueRead(reply);
        auto clock = fastecu::MakeAutoAdvancingClock(10ms);
        fastecu::FakeCancellationToken cancellation;
        auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, true);
        ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
        auto& protocol = *protocol_result;
        EXPECT_THAT(protocol.Start(cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
    }
}

TEST(SsmLoggingProtocolTest, ConsecutiveContinuousFramesAreNotConcatenated)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    const auto request = BuildRequest(bytes::Bytes{0xa8, 1, 0, 0x10, 0});
    transport->ExpectWrite(request);
    transport->ExpectWrite(request);
    transport->QueueRead(BuildResponse(bytes::Bytes{42}));
    transport->QueueRead(BuildResponse(bytes::Bytes{99}));
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto first = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_EQ(first->samples.size(), 1U);
    EXPECT_EQ(first->samples.front().raw_value, "42");
    const auto second = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(second, fastecu::testing::IsOk());
    ASSERT_EQ(second->samples.size(), 1U);
    EXPECT_EQ(second->samples.front().raw_value, "99");
    EXPECT_TRUE(script->ScriptConsumed());
}

TEST(SsmLoggingProtocolTest, CoalescedContinuousFramesRetainTheNextFrame)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    const auto request = BuildRequest(bytes::Bytes{0xa8, 1, 0, 0x10, 0});
    transport->ExpectWrite(request);
    transport->ExpectWrite(request);
    auto responses = BuildResponse(bytes::Bytes{42});
    const auto next = BuildResponse(bytes::Bytes{99});
    responses.insert(responses.end(), next.begin(), next.end());
    transport->QueueRead(responses);
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(1ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto first = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(first, fastecu::testing::IsOk());
    ASSERT_EQ(first->samples.size(), 1U);
    EXPECT_EQ(first->samples.front().raw_value, "42");
    const auto second = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(second, fastecu::testing::IsOk());
    ASSERT_EQ(second->samples.size(), 1U);
    EXPECT_EQ(second->samples.front().raw_value, "99");
    EXPECT_TRUE(script->ScriptConsumed());
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, GetParam());
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, GetParam());
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, GetParam());
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto rejected = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(rejected, fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, false, GetParam());
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, false, GetParam());
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto result = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
}

TEST_P(SsmFrameIntegrityTest, StartAcceptsExactlyOneProbeByteFromTcu)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}, false));
    // Hand-checked sum: 0x80 + 0xf0 + 0x18 + 2 + 0xe8 + 7 = 0x279.
    transport->QueueRead(bytes::Bytes{0x80, 0xf0, 0x18, 2, 0xe8, 7, 0x79});
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, false, GetParam());
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, false, GetParam());
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, GetParam());
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
    const auto result = protocol.Poll(50ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;
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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

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
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, false);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    const auto result = protocol.Poll(14ms, cancellation);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1U);
    EXPECT_EQ(result->samples[0].raw_value, "42");
}

INSTANTIATE_TEST_SUITE_P(DirectAndOpenPort, SsmFrameIntegrityTest, ::testing::Bool(),
                         ([](const auto& param_info) { return param_info.param ? "OpenPort" : "Direct"; }));

TEST(SsmLoggingProtocolTest, RejectsNonfiniteFloatWithoutFabricatedZero)
{
    for (const auto& payload : {bytes::Bytes{0x7f, 0x80, 0, 0}, bytes::Bytes{0x7f, 0xc0, 0, 0}})
    {
        SCOPED_TRACE(::testing::PrintToString(payload));
        auto channel = Channel("float", 0x1000, 4);
        channel.storage = fastecu::logging::NumericStorage::kFloat;
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0, 0x10, 0, 0, 0x10, 1, 0, 0x10, 2, 0, 0x10, 3}));
        transport->QueueRead(BuildResponse(payload));
        auto clock = fastecu::MakeAutoAdvancingClock(10ms);
        fastecu::FakeCancellationToken cancellation;
        auto protocol_result = MakeProtocol(clock, std::move(transport), {channel}, true, true);
        ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
        auto& protocol = *protocol_result;
        EXPECT_THAT(protocol.Poll(50ms, cancellation), fastecu::testing::IsErr(fastecu::ErrorKind::kBadResponse));
    }
}

TEST(SsmLoggingProtocolTest, OpenPort2PollCancelledAfterWriteSkipsTheRead)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->ExpectWrite(BuildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    auto *script = transport.get();
    auto clock = fastecu::MakeAutoAdvancingClock(10ms);
    fastecu::FakeCancellationToken cancellation;
    // The readiness check passes; the read's own check cancels.
    cancellation.CancelOnCheck(2);
    auto protocol_result = MakeProtocol(clock, std::move(transport), {Channel()}, true, true);
    ASSERT_THAT(protocol_result, fastecu::testing::IsOk());
    auto& protocol = *protocol_result;

    EXPECT_THAT(protocol.Poll(50ms, cancellation),
                fastecu::testing::IsErrWith(fastecu::ErrorKind::kCancelled, "SSM logging cancelled"));
    EXPECT_TRUE(script->ScriptConsumed());
}
