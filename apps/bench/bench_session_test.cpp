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

constexpr std::uint32_t kRequestId = 0x7E0;
constexpr std::uint32_t kResponseId = 0x7E8;
const bytes::Bytes kSeed{0x12, 0x34, 0x56, 0x78};
const bytes::Bytes kVendorSeed{0xDE, 0xAD, 0xBE, 0xEF};

bytes::Bytes request(bytes::ByteView pdu)
{
    return bytes::composeBe(kRequestId, pdu);
}

bytes::Bytes response(bytes::ByteView pdu)
{
    return bytes::composeBe(kResponseId, pdu);
}

struct Harness
{
    flash::ScriptedCanFlashTransport *transport = nullptr;
    FakeClock clock = make_auto_advancing_clock(1);
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

    void expectSession(bytes::Byte echoed_session = MitsuColtCan::kSessionBootload)
    {
        transport->expectWrite(request(MitsuColtCan::buildDiagnosticSession(MitsuColtCan::kSessionBootload)));
        transport->queueRead(response(bytes::Bytes{0x50, echoed_session}));
    }

    void expectSeed(bytes::Byte echoed_level = 0x05)
    {
        transport->expectWrite(request(MitsuColtCan::buildSecurityAccessSeedRequest()));
        transport->queueRead(response(bytes::composeBe(bytes::Byte{0x67}, echoed_level, kSeed)));
    }

    void expectKey(bytes::Byte echoed_level = 0x06)
    {
        transport->expectWrite(request(MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(kSeed))));
        transport->queueRead(response(bytes::Bytes{0x67, echoed_level}));
    }

    void expectBasicSession(bytes::Byte echoed_session = MitsuColtCan::kSessionBasic)
    {
        transport->expectWrite(request(MitsuColtCan::buildDiagnosticSession(MitsuColtCan::kSessionBasic)));
        transport->queueRead(response(bytes::Bytes{0x50, echoed_session}));
    }

    void expectVendorSeed()
    {
        transport->expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
        transport->queueRead(
            response(bytes::composeBe(bytes::Byte{0x63}, MitsuColtCanVendorExt::kVendorChallengeSelector,
                                      MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction, kVendorSeed)));
    }

    void expectVendorKey(bytes::Byte accepted = MitsuColtCanVendorExt::kVendorChallengeAccepted)
    {
        const std::uint32_t key =
            MitsuColtCanVendorExt::challengeInverseTransform(MitsuColtCanVendorExt::bytesToSeed(kVendorSeed));
        transport->expectWrite(request(MitsuColtCanVendorExt::buildChallengeKey(key)));
        transport->queueRead(response(bytes::Bytes{0x63, MitsuColtCanVendorExt::kVendorChallengeSelector, accepted}));
    }
};

TEST(BenchSession, ConnectSendsTheExactThreeHandshakePdusOnceAndRecordsEvidence)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey();

    const Status result = harness.session->connect();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(harness.transport->scriptConsumed());
    const TrafficEvidence& traffic = harness.session->last_traffic();
    EXPECT_EQ(traffic.exchange_count, 3u);
    EXPECT_EQ(traffic.tx, MitsuColtCan::buildDiagnosticSession(MitsuColtCan::kSessionBootload));
    EXPECT_EQ(traffic.rx, (bytes::Bytes{0x50, MitsuColtCan::kSessionBootload}));
    EXPECT_EQ(traffic.last_tx, MitsuColtCan::buildSecurityAccessKey(MitsuColtCan::seedKey(kSeed)));
    EXPECT_EQ(traffic.last_rx, (bytes::Bytes{0x67, 0x06}));
    EXPECT_GT(traffic.elapsed_ms, 0u);
}

TEST(BenchSession, ConnectRejectsAWrongPositiveSessionEcho)
{
    Harness harness;
    harness.expectSession(MitsuColtCan::kSessionBasic);

    const Status result = harness.session->connect();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(harness.session->last_traffic().rx, (bytes::Bytes{0x50, MitsuColtCan::kSessionBasic}));
}

TEST(BenchSession, ConnectRejectsAWrongPositiveSeedLevelEcho)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed(0x04);

    const Status result = harness.session->connect();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
}

TEST(BenchSession, ConnectRejectsAWrongPositiveKeyLevelEcho)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey(0x07);

    const Status result = harness.session->connect();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
}

TEST(BenchSession, VendorChallengeIsSkippedWhenNotRequested)
{
    Harness harness;
    harness.expectSession();
    harness.expectSeed();
    harness.expectKey();

    const Status result = harness.session->connect();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(harness.transport->scriptConsumed());
    EXPECT_EQ(harness.session->last_traffic().exchange_count, 3u);
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

    const Status result = harness.session->connect();

    ASSERT_TRUE(result.has_value());
    // ScriptedCanFlashTransport rejects any write that does not match the next
    // expectation in order, so a green script IS the ordering assertion.
    EXPECT_TRUE(harness.transport->scriptConsumed());
    EXPECT_EQ(harness.session->last_traffic().exchange_count, 6u);
}

TEST(BenchSession, VendorChallengeRejectsAKeyReplyThatOnlyEchoesTheSelector)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.expectVendorSeed();
    // 0x00 in place of kVendorChallengeAccepted: the selector still echoes,
    // but the ECU has not granted the transition.
    harness.expectVendorKey(0x00);

    const Status result = harness.session->connect();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
}

TEST(BenchSession, VendorChallengeRejectsAKeyReplyThatOnlyEchoesAcceptance)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.expectVendorSeed();
    const std::uint32_t key =
        MitsuColtCanVendorExt::challengeInverseTransform(MitsuColtCanVendorExt::bytesToSeed(kVendorSeed));
    harness.transport->expectWrite(request(MitsuColtCanVendorExt::buildChallengeKey(key)));
    // 0x00 in place of kVendorChallengeSelector: kVendorChallengeAccepted is
    // present, but byte 0 does not echo the selector back.
    harness.transport->queueRead(response(bytes::Bytes{0x63, 0x00, MitsuColtCanVendorExt::kVendorChallengeAccepted}));

    const Status result = harness.session->connect();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
}

TEST(BenchSession, VendorChallengeRejectsAShortSeedReply)
{
    Harness harness{true};
    harness.expectBasicSession();
    harness.transport->expectWrite(request(MitsuColtCanVendorExt::buildChallengeSeedRequest()));
    // Selector bytes present but only two seed bytes behind them.
    harness.transport->queueRead(
        response(bytes::Bytes{0x63, MitsuColtCanVendorExt::kVendorChallengeSelector,
                              MitsuColtCanVendorExt::kVendorChallengeSeedSubfunction, 0xDE, 0xAD}));

    const Status result = harness.session->connect();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
}

} // namespace
} // namespace fastecu::bench
