#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/result_matchers.h"
#include "apps/bench/bench_session.h"

#include <gtest/gtest.h>

#include <memory>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::bench
{
namespace
{

using namespace std::chrono_literals;

constexpr std::uint32_t kRequestId = 0x7E0;
constexpr std::uint32_t kResponseId = 0x7E8;
const bytes::Bytes kSeed{0x12, 0x34, 0x56, 0x78};
const bytes::Bytes kVendorSeed{0xDE, 0xAD, 0xBE, 0xEF};

bytes::Bytes request(bytes::ByteView pdu)
{
    return bytes::ComposeBe(kRequestId, pdu);
}

bytes::Bytes response(bytes::ByteView pdu)
{
    return bytes::ComposeBe(kResponseId, pdu);
}

struct Harness
{
    flash::ScriptedCanFlashTransport *transport = nullptr;
    FakeClock clock = MakeAutoAdvancingClock(1ms);
    RecordingEventSink events;
    FakeCancellationToken cancellation;
    std::unique_ptr<BenchSession> session;

    explicit Harness(bool vendor_challenge = false)
    {
        auto owned = std::make_unique<flash::ScriptedCanFlashTransport>();
        transport = owned.get();
        session = std::make_unique<BenchSession>(std::move(owned), kRequestId, kResponseId, clock, events, cancellation,
                                                 vendor_challenge);
    }

    void expectSession(bytes::Byte echoed_session = mitsu_colt_can::kSessionBootload)
    {
        transport->ExpectWrite(request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)));
        transport->QueueRead(response(bytes::Bytes{0x50, echoed_session}));
    }

    void expectSeed(bytes::Byte echoed_level = 0x05)
    {
        transport->ExpectWrite(request(mitsu_colt_can::BuildSecurityAccessSeedRequest()));
        transport->QueueRead(response(bytes::ComposeBe(bytes::Byte{0x67}, echoed_level, kSeed)));
    }

    void expectKey(bytes::Byte echoed_level = 0x06)
    {
        transport->ExpectWrite(request(mitsu_colt_can::BuildSecurityAccessKey(mitsu_colt_can::SeedKey(kSeed))));
        transport->QueueRead(response(bytes::Bytes{0x67, echoed_level}));
    }

    void expectBasicSession(bytes::Byte echoed_session = mitsu_colt_can::kSessionBasic)
    {
        transport->ExpectWrite(request(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBasic)));
        transport->QueueRead(response(bytes::Bytes{0x50, echoed_session}));
    }

    void expectVendorSeed()
    {
        transport->ExpectWrite(request(mitsu_colt_can_vendor_ext::BuildChallengeSeedRequest()));
        transport->QueueRead(
            response(bytes::ComposeBe(bytes::Byte{0x63}, mitsu_colt_can_vendor_ext::kVendorChallengeSelector,
                                      mitsu_colt_can_vendor_ext::kVendorChallengeSeedSubfunction, kVendorSeed)));
    }

    void expectVendorKey(bytes::Byte accepted = mitsu_colt_can_vendor_ext::kVendorChallengeAccepted)
    {
        const std::uint32_t key =
            mitsu_colt_can_vendor_ext::ChallengeInverseTransform(mitsu_colt_can_vendor_ext::BytesToSeed(kVendorSeed));
        transport->ExpectWrite(request(mitsu_colt_can_vendor_ext::BuildChallengeKey(key)));
        transport->QueueRead(
            response(bytes::Bytes{0x63, mitsu_colt_can_vendor_ext::kVendorChallengeSelector, accepted}));
    }
};

TEST(BenchSession, ConnectSendsTheExactThreeHandshakePdusOnceAndRecordsEvidence)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey();

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsOk());
    EXPECT_TRUE(harness.transport->ScriptConsumed());
    const TrafficEvidence& traffic = harness.session->last_traffic();
    EXPECT_EQ(traffic.exchange_count, 3U);
    EXPECT_THAT(traffic.tx,
                test_bytes::BytesEq(mitsu_colt_can::BuildDiagnosticSession(mitsu_colt_can::kSessionBootload)));
    EXPECT_THAT(traffic.rx, test_bytes::BytesEq((bytes::Bytes{0x50, mitsu_colt_can::kSessionBootload})));
    EXPECT_THAT(traffic.last_tx,
                test_bytes::BytesEq(mitsu_colt_can::BuildSecurityAccessKey(mitsu_colt_can::SeedKey(kSeed))));
    EXPECT_THAT(traffic.last_rx, test_bytes::BytesEq((bytes::Bytes{0x67, 0x06})));
    EXPECT_GT(traffic.elapsed_ms, 0U);
}

TEST(BenchSession, ConnectRejectsAWrongPositiveSessionEcho)
{
    Harness harness;
    harness.expectSession(mitsu_colt_can::kSessionBasic);

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(harness.session->last_traffic().rx, (bytes::Bytes{0x50, mitsu_colt_can::kSessionBasic}));
}

TEST(BenchSession, ConnectRejectsAWrongPositiveSeedLevelEcho)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed(0x04);

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(BenchSession, ConnectRejectsAWrongPositiveKeyLevelEcho)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey(0x07);

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(BenchSession, VendorChallengeIsSkippedWhenNotRequested)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey();

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsOk());
    EXPECT_TRUE(harness.transport->ScriptConsumed());
    EXPECT_EQ(harness.session->last_traffic().exchange_count, 3U);
}

TEST(BenchSession, VendorChallengePrecedesTheBootloadSessionInOrder)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.expectVendorSeed();
    harness.expectVendorKey();
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey();

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsOk());
    // ScriptedCanFlashTransport rejects any write that does not match the next
    // expectation in order, so a green script IS the ordering assertion.
    EXPECT_TRUE(harness.transport->ScriptConsumed());
    EXPECT_EQ(harness.session->last_traffic().exchange_count, 6U);
}

TEST(BenchSession, VendorChallengeRejectsAKeyReplyThatOnlyEchoesTheSelector)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.expectVendorSeed();
    // 0x00 in place of kVendorChallengeAccepted: the selector still echoes,
    // but the ECU has not granted the transition.
    harness.expectVendorKey(0x00);

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(BenchSession, VendorChallengeRejectsAKeyReplyThatOnlyEchoesAcceptance)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.expectVendorSeed();
    const std::uint32_t key =
        mitsu_colt_can_vendor_ext::ChallengeInverseTransform(mitsu_colt_can_vendor_ext::BytesToSeed(kVendorSeed));
    harness.transport->ExpectWrite(request(mitsu_colt_can_vendor_ext::BuildChallengeKey(key)));
    // 0x00 in place of kVendorChallengeSelector: kVendorChallengeAccepted is
    // present, but byte 0 does not echo the selector back.
    harness.transport->QueueRead(
        response(bytes::Bytes{0x63, 0x00, mitsu_colt_can_vendor_ext::kVendorChallengeAccepted}));

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

TEST(BenchSession, VendorChallengeRejectsAShortSeedReply)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.transport->ExpectWrite(request(mitsu_colt_can_vendor_ext::BuildChallengeSeedRequest()));
    // Selector bytes present but only two seed bytes behind them.
    harness.transport->QueueRead(
        response(bytes::Bytes{0x63, mitsu_colt_can_vendor_ext::kVendorChallengeSelector,
                              mitsu_colt_can_vendor_ext::kVendorChallengeSeedSubfunction, 0xDE, 0xAD}));

    ASSERT_THAT(harness.session->connect(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
}

} // namespace
} // namespace fastecu::bench
