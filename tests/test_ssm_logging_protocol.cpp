#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "scripted_ssm_transport.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"

namespace
{
using fastecu::logging::LoggingChannel;
using fastecu::logging::RawAssembly;
using fastecu::logging::SsmLoggingProtocol;

class FakeClock final : public fastecu::IClock
{
  public:
    std::uint64_t now_ms() const override
    {
        const std::uint64_t value = now_;
        now_ += kStepMs;
        return value;
    }

    fastecu::Status sleep(int, const fastecu::ICancellationToken&) override
    {
        now_ += kStepMs;
        return {};
    }

  private:
    static constexpr std::uint64_t kStepMs = 10;
    mutable std::uint64_t now_ = 0;
};

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

class CancelsDuringSecondRead final : public fastecu::ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return ++checks_ >= 4;
    }

  private:
    mutable int checks_ = 0;
};

LoggingChannel channel(std::string id = "rpm", std::uint32_t address = 0x1000,
                       std::size_t length = 1)
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
    bytes::Bytes message = {
        0x80, 0xf0, 0x10, static_cast<bytes::Byte>(payload.size() + 1), 0xe8};
    message.insert(message.end(), payload.begin(), payload.end());
    message.push_back(SsmProtocol::checksum(message));
    return message;
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
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->queueRead(buildResponse(bytes::Bytes{0, 0, 0}));
    transport->queue_no_frame();
    auto *script = transport.get();
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true, false);

    const auto result = protocol.start(cancellation);

    ASSERT_TRUE(result);
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(SsmLoggingProtocolTest, StartReturnsBadResponseForShortReply)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    transport->queueRead(bytes::Bytes{});
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true, true);

    const auto result = protocol.start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
}

TEST(SsmLoggingProtocolTest, StartReturnsBadResponseForNegativeReply)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0x07}));
    auto response = buildResponse(bytes::Bytes{0, 0, 0});
    response[4] = 0x7f;
    transport->queueRead(response);
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true,
                                true);

    const auto result = protocol.start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::BadResponse);
}

TEST(SsmLoggingProtocolTest, StartFailsWhenAdapterIsClosed)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->setOpen(false);
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true, false);

    const auto result = protocol.start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "adapter disconnected");
}

TEST(SsmLoggingProtocolTest, PreservesDecimalByteConcatenation)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queueRead(buildResponse(bytes::Bytes{16, 16}));
    transport->queue_no_frame();
    auto *script = transport.get();
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(
        clock, std::move(transport), {channel("rpm", 0x1000, 2)}, true, false);

    const auto result = protocol.poll(50, cancellation);

    ASSERT_TRUE(result);
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 1u);
    EXPECT_EQ(result->samples[0].channel_id, "rpm");
    EXPECT_EQ(result->samples[0].raw_value, "1616");
    EXPECT_TRUE(script->scriptConsumed());
    EXPECT_TRUE(script->ok());
}

TEST(SsmLoggingProtocolTest, PollPreservesChannelRequestOrder)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{
        0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
    transport->queueRead(buildResponse(bytes::Bytes{42, 99}));
    transport->queue_no_frame();
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(
        clock, std::move(transport),
        {channel("first", 0x1000), channel("second", 0x1003)}, true, false);

    const auto result = protocol.poll(50, cancellation);

    ASSERT_TRUE(result);
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2u);
    EXPECT_EQ(result->samples[0].channel_id, "first");
    EXPECT_EQ(result->samples[0].raw_value, "42");
    EXPECT_EQ(result->samples[1].channel_id, "second");
    EXPECT_EQ(result->samples[1].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, PollHonorsSnapshottedHistoricalResponseOffsets)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(buildRequest(bytes::Bytes{
        0xA8, 0x01, 0x00, 0x10, 0x00, 0x00, 0x10, 0x03}));
    transport->queueRead(buildResponse(bytes::Bytes{42, 77, 99}));
    transport->queue_no_frame();
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(
        clock, std::move(transport),
        {channel("first", 0x1000), channel("third", 0x1003)},
        std::vector<std::size_t>{0, 2}, true, false);

    const auto result = protocol.poll(50, cancellation);

    ASSERT_TRUE(result);
    ASSERT_TRUE(result->responded);
    ASSERT_EQ(result->samples.size(), 2u);
    EXPECT_EQ(result->samples[0].raw_value, "42");
    EXPECT_EQ(result->samples[1].raw_value, "99");
}

TEST(SsmLoggingProtocolTest, PollReturnsNoResponseOnDirectReadTimeout)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queue_no_frame();
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true, true);

    const auto result = protocol.poll(50, cancellation);

    ASSERT_TRUE(result);
    EXPECT_FALSE(result->responded);
    EXPECT_TRUE(result->samples.empty());
}

TEST(SsmLoggingProtocolTest, HeaderResynchronizationRemainsDeadlineBounded)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queueRead(bytes::Bytes(64, 0x01));
    queueNoFrames(*transport, 8);
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true, false);

    const auto result = protocol.poll(100, cancellation);

    ASSERT_TRUE(result);
    EXPECT_FALSE(result->responded);
}

TEST(SsmLoggingProtocolTest, CancellationDuringFramingReturnsCancelled)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queueRead(bytes::Bytes{0x80});
    transport->queueRead(bytes::Bytes{0xf0, 0x10});
    FakeClock clock;
    CancelsDuringSecondRead cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true, false);

    const auto result = protocol.poll(50, cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
}

TEST(SsmLoggingProtocolTest, StartCancellationReturnsCancelledWithoutIo)
{
    FakeClock clock;
    Cancelled cancellation;
    SsmLoggingProtocol protocol(
        clock, std::make_unique<ScriptedSsmTransport>(), {channel()}, true, false);

    const auto result = protocol.start(cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
}

TEST(SsmLoggingProtocolTest, PollPropagatesTypedWriteFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queue_write_error(fastecu::ErrorKind::Disconnected,
                                 "sentinel SSM write disconnect");
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true, false);

    const auto result = protocol.poll(50, cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
    EXPECT_EQ(result.error().detail, "sentinel SSM write disconnect");
}

TEST(SsmLoggingProtocolTest, PollPropagatesTypedReadFailure)
{
    auto transport = std::make_unique<ScriptedSsmTransport>();
    transport->expectWrite(
        buildRequest(bytes::Bytes{0xA8, 0x01, 0x00, 0x10, 0x00}));
    transport->queue_error(fastecu::ErrorKind::Internal,
                           "sentinel SSM read failure");
    FakeClock clock;
    NeverCancelled cancellation;
    SsmLoggingProtocol protocol(clock, std::move(transport), {channel()}, true, false);

    const auto result = protocol.poll(50, cancellation);

    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Internal);
    EXPECT_EQ(result.error().detail, "sentinel SSM read failure");
}
