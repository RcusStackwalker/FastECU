#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/mock_clock.h"
#include "src/backend/ports/testing/recording_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/protocol/uds/uds_client.h"

namespace fastecu::flash
{
namespace
{

// Two layers of assertion here, and they catch different mistakes.
//
// The table tests below pin the byte values, transcribed a second time from
// the same legacy generate_can_seed_key/encrypt_payload/decrypt_payload lines
// the header cites, so a slipped digit in the header fails here.
//
// The vector tests pin what those tables actually PRODUCE when run through
// SsmProtocol. They exist because the executor suites can compare the
// executor's crypto against an independent literal transcript while still
// exercising the same SsmProtocol implementation: a change to Feistel
// arithmetic could otherwise move derived fixtures and wire output together.
// These fixed vectors do not move. They were
// computed by an independent reimplementation of transformWord() from
// src/algorithms/protocol/ssm/ssm_protocol_core.cpp and then confirmed
// against the compiled implementation; they are a regression pin, not an
// ECU-sourced golden -- no legacy source publishes a test vector.

TEST(DensoIso15765CanCommonTest, SeedKeyTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 16> kExpected{0x78B1, 0x4625, 0x201C, 0x9EA5, 0xAD6B, 0x35F4, 0xFD21, 0x5E71,
                                                      0xB046, 0x7F4A, 0x4B75, 0x93F9, 0x1895, 0x8961, 0x3ECC, 0x862B};
    EXPECT_EQ(kDensoIso15765SeedKeyTable, kExpected);
}

TEST(DensoIso15765CanCommonTest, EncryptTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 4> kExpected{0xC85B, 0x32C0, 0xE282, 0x92A0};
    EXPECT_EQ(kDensoIso15765EncryptTable, kExpected);
}

TEST(DensoIso15765CanCommonTest, DecryptTableMatchesLegacyValues)
{
    constexpr std::array<std::uint16_t, 4> kExpected{0x92A0, 0xE282, 0x32C0, 0xC85B};
    EXPECT_EQ(kDensoIso15765DecryptTable, kExpected);
}

// The applicable legacy sources spell the decrypt table out rather than
// deriving it from the encrypt table, so the reversal relationship
// calculatePayload relies on to invert is pinned here rather than assumed.
// The BEEF-dialect diesel read path is raw and therefore is not a decrypt consumer.
TEST(DensoIso15765CanCommonTest, DecryptTableIsEncryptTableReversed)
{
    ASSERT_EQ(kDensoIso15765EncryptTable.size(), kDensoIso15765DecryptTable.size());
    for (std::size_t i = 0; i < kDensoIso15765EncryptTable.size(); ++i)
    {
        EXPECT_EQ(kDensoIso15765EncryptTable[i], kDensoIso15765DecryptTable[kDensoIso15765EncryptTable.size() - 1 - i]);
    }
}

TEST(DensoIso15765CanCommonTest, SeedKeyProducesKnownVectors)
{
    const bytes::Bytes seed_a{0x11, 0x22, 0x33, 0x44};
    EXPECT_THAT(
        ssm_protocol::CalculateSeedKey(seed_a, kDensoIso15765SeedKeyTable, ssm_protocol::kIndexTransformationStock),
        test_bytes::BytesEq((bytes::Bytes{0x35, 0xB6, 0x83, 0xBF})));

    const bytes::Bytes seed_b{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_THAT(
        ssm_protocol::CalculateSeedKey(seed_b, kDensoIso15765SeedKeyTable, ssm_protocol::kIndexTransformationStock),
        test_bytes::BytesEq((bytes::Bytes{0xB6, 0xF5, 0x24, 0x21})));
}

TEST(DensoIso15765CanCommonTest, EncryptProducesKnownPayloadVector)
{
    const bytes::Bytes plain{0x00, 0x01, 0x02, 0x03, 0xFC, 0xFD, 0xFE, 0xFF};
    EXPECT_THAT(ssm_protocol::CalculatePayload(plain, static_cast<std::uint32_t>(plain.size()),
                                               kDensoIso15765EncryptTable, ssm_protocol::kIndexTransformationStock),
                test_bytes::BytesEq((bytes::Bytes{0xE0, 0xD3, 0x85, 0x2B, 0xC5, 0xFE, 0x4B, 0x39})));
}

// The dump path decrypts each 256-byte page with the decrypt table; the write
// path encrypts the whole image with the encrypt table. A page that survives
// encrypt-then-decrypt unchanged is what makes reading back a freshly written
// ROM meaningful, so pin the round trip and not only the one direction.
TEST(DensoIso15765CanCommonTest, DecryptInvertsEncrypt)
{
    const bytes::Bytes cipher{0xE0, 0xD3, 0x85, 0x2B, 0xC5, 0xFE, 0x4B, 0x39};
    EXPECT_THAT(ssm_protocol::CalculatePayload(cipher, static_cast<std::uint32_t>(cipher.size()),
                                               kDensoIso15765DecryptTable, ssm_protocol::kIndexTransformationStock),
                test_bytes::BytesEq((bytes::Bytes{0x00, 0x01, 0x02, 0x03, 0xFC, 0xFD, 0xFE, 0xFF})));
}

using namespace std::chrono_literals;
using fastecu::ErrorKind;
using fastecu::LogLevel;
using fastecu::ManualCancellationToken;
using fastecu::MockClock;
using fastecu::RecordingClock;
using fastecu::RecordingEventSink;
using fastecu::Status;

bytes::Bytes Request(std::initializer_list<bytes::Byte> payload)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, 0x7E0);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bytes::Bytes Response(std::initializer_list<bytes::Byte> payload)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, 0x7E8);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Cancels the operator's token once a given log line has been emitted, so a
// test can stop the flow at an exact point between two exchanges.
class CancellingEventSink : public RecordingEventSink
{
  public:
    CancellingEventSink(ManualCancellationToken& token, std::string_view trigger) : token_(token), trigger_(trigger)
    {
    }
    void Log(LogLevel level, std::string_view message) override
    {
        RecordingEventSink::Log(level, message);
        if (message == trigger_)
        {
            token_.Cancel();
        }
    }

  private:
    ManualCancellationToken& token_;
    std::string_view trigger_;
};

// The real UDS stack over the scripted transport, on the 0x7E0/0x7E8 pair.
struct CommonFixture
{
    ScriptedCanFlashTransport transport;
    CanFlashUdsChannel channel{transport, 0x7E0, 0x7E8};
    RecordingClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    uds::UdsClient client{channel, clock, events};
    CanExecutorContext ctx{cancellation, events, clock, client, channel};
};

TEST(DensoIso15765CanCommonTest, SecurityAccessReadsLiteralSeedAndKeyAtTwoSeconds)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    f.transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));

    EXPECT_THAT(DensoSecurityAccess(f.ctx), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms, 2000ms));
}

TEST(DensoIso15765CanCommonTest, SecurityAccessRereadsPendingReplyWithoutResending)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x27, 0x61}), Response({0x7F, 0x27, 0x78}));
    f.transport.QueueRead(Response({0x7F, 0x27, 0x78}));
    f.transport.QueueRead(Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    f.transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));

    EXPECT_THAT(DensoSecurityAccess(f.ctx), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(2000ms, 3000ms, 3000ms, 2000ms));
}

TEST(DensoIso15765CanCommonTest, SecurityAccessRejectsShortSeedWithoutSendingKey)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33}));

    const Status result = DensoSecurityAccess(f.ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_EQ(f.transport.WritesConsumed(), 1U);
}

TEST(DensoIso15765CanCommonTest, SecurityAccessPropagatesCancellation)
{
    CommonFixture f;
    CancellingEventSink events(f.cancellation, "Seed request ok");
    uds::UdsClient client(f.channel, f.clock, events);
    CanExecutorContext ctx{f.cancellation, events, f.clock, client, f.channel};
    f.transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));

    const Status result = DensoSecurityAccess(ctx);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
    EXPECT_EQ(f.transport.WritesConsumed(), 1U);
}

const bytes::Bytes kSetupPdu{0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00};

void ScriptEraseSetup(ScriptedCanFlashTransport& transport)
{
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
                       Response({0x74, 0x20, 0x01, 0x05}));
}

void ScriptEraseTrigger(ScriptedCanFlashTransport& transport)
{
    transport.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
}

TEST(DensoIso15765CanCommonTest, EraseSetupMismatchDoesNotSendTrigger)
{
    CommonFixture f;
    f.transport.Exchange(Request({0x34, 0x04, 0x44, 0x08, 0xFA, 0xC0, 0x00, 0x00, 0x17, 0x3F, 0x00}),
                         Response({0x74, 0x20, 0x01, 0x04}));

    const Status result = DensoIso15765Erase(f.ctx, kSetupPdu);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_EQ(f.transport.WritesConsumed(), 1U);
}

TEST(DensoIso15765CanCommonTest, EraseAccepts71_01_02AfterPolling)
{
    CommonFixture f;
    ScriptEraseSetup(f.transport);
    ScriptEraseTrigger(f.transport);
    f.transport.QueueRead(Response({0x71, 0x01, 0x03}));
    f.transport.QueueRead(Response({0x71, 0x01, 0x02, 0x00}));

    EXPECT_THAT(DensoIso15765Erase(f.ctx, kSetupPdu), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.WritesConsumed(), 2U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::ElementsAre(500ms, 500ms, 500ms));
    EXPECT_THAT(f.clock.sleep_calls, ::testing::ElementsAre(500ms, 500ms));
    EXPECT_THAT(f.events.logs,
                ::testing::ElementsAre(
                    ::testing::Pair(LogLevel::kInfo, "Setting flash start & length"),
                    ::testing::Pair(LogLevel::kInfo, "Erasing ECU ROM"),
                    ::testing::Pair(LogLevel::kInfo, "Flash erased! Starting flash write, do not power off!")));
}

TEST(DensoIso15765CanCommonTest, ErasePollingStopsAfter20ReceivesAndNeverResendsTrigger)
{
    CommonFixture f;
    ScriptEraseSetup(f.transport);
    ScriptEraseTrigger(f.transport);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        f.transport.QueueRead(Response({0x71, 0x01, 0x03}));
    }

    const Status result = DensoIso15765Erase(f.ctx, kSetupPdu);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kBadResponse);
    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.WritesConsumed(), 2U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::Each(500ms));
    EXPECT_EQ(f.transport.ReadTimeouts().size(), 21U);
    EXPECT_THAT(f.clock.sleep_calls, ::testing::Each(500ms));
    EXPECT_EQ(f.clock.sleep_calls.size(), 21U);
    EXPECT_THAT(f.events.logs, ::testing::Contains(::testing::Pair(LogLevel::kError, "Flash area erase failed")));
}

TEST(DensoIso15765CanCommonTest, EraseCancellationAfterTriggerStopsPolling)
{
    CommonFixture f;
    // Stop the operator's token during the settle sleep that follows the trigger.
    ::testing::NiceMock<MockClock> clock;
    EXPECT_CALL(clock, Sleep).WillOnce(::testing::DoAll([&] { f.cancellation.Cancel(); }, clock.SleepOnFake()));
    uds::UdsClient client(f.channel, clock, f.events);
    CanExecutorContext ctx{f.cancellation, f.events, clock, client, f.channel};
    ScriptEraseSetup(f.transport);
    ScriptEraseTrigger(f.transport);

    const Status result = DensoIso15765Erase(ctx, kSetupPdu);

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::kCancelled);
    EXPECT_EQ(f.transport.WritesConsumed(), 2U);
    EXPECT_EQ(f.transport.ReadTimeouts().size(), 1U);
}

bytes::Bytes RequestTo(std::uint32_t id, std::initializer_list<bytes::Byte> payload)
{
    bytes::Bytes out;
    bytes::AppendU32Be(out, id);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Both N83M families send this literal run, with the in-car 0x7E1 request
// asking for subfunction 0x63. Replies are read and thrown away, so each one
// here carries a deliberately wrong id and content.
TEST(DensoIso15765CanCommonTest, N83mInCarSequencePreservesBothFamilyTranscripts)
{
    CommonFixture f;
    const std::array<bytes::Bytes, 10> requests{RequestTo(0x7A2, {0x10, 0xC0}), RequestTo(0x7E0, {0x10, 0x63}),
                                                RequestTo(0x7DF, {0x10, 0x03}), RequestTo(0x7E1, {0x10, 0x63}),
                                                RequestTo(0x7B0, {0x10, 0x03}), RequestTo(0x7B0, {0x85, 0x02}),
                                                RequestTo(0x7DF, {0x85, 0x02}), RequestTo(0x7B0, {0x85, 0x02}),
                                                RequestTo(0x7DF, {0x85, 0x02}), RequestTo(0x7DF, {0x28, 0x03, 0x01})};
    for (std::size_t i = 0; i < requests.size(); ++i)
    {
        f.transport.Exchange(requests[i], RequestTo(0x123 + static_cast<std::uint32_t>(i), {0x7F, 0xEE, 0xEE}));
    }

    EXPECT_THAT(N83mInCarFireAndForget(f.ctx, f.transport), fastecu::testing::IsOk());

    EXPECT_TRUE(f.transport.ScriptConsumed());
    EXPECT_EQ(f.transport.WritesConsumed(), 10U);
    EXPECT_EQ(f.transport.ReadTimeouts().size(), 10U);
    EXPECT_THAT(f.transport.ReadTimeouts(), ::testing::Each(200ms));
}

} // namespace
} // namespace fastecu::flash
