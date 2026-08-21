#include "apps/bench/bench_session.h"

#include <gtest/gtest.h>

#include <memory>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
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

    Harness()
    {
        auto owned = std::make_unique<flash::ScriptedCanFlashTransport>();
        transport = owned.get();
        session =
            std::make_unique<BenchSession>(std::move(owned), kRequestId, kResponseId, clock, events, cancellation);
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

} // namespace
} // namespace fastecu::bench
