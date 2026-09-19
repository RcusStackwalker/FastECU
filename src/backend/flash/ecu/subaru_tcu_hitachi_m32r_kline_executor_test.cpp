#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h"

#include <gtest/gtest.h>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace
{
using namespace fastecu;
using namespace fastecu::flash;

bytes::Bytes frame(bytes::Bytes payload)
{
    return SsmProtocol::addHeader(payload, 0xf0, 0x18);
}

bytes::Bytes idResponse()
{
    return {0x80, 0xf0, 0x18, 0x09, 0xff, 0, 0, 0, 0x12, 0x34, 0x56, 0x78, 0x9a, 0};
}

bytes::Bytes seedResponse()
{
    return {0x80, 0xf0, 0x18, 0x06, 0x67, 0x01, 0xde, 0xad, 0xbe, 0xef, 0};
}

bytes::Bytes expectedSeedKey()
{
    static constexpr std::array<std::uint16_t, 16> index = {0x0FE9, 0xCA58, 0x5E90, 0xDFF1, 0x690B, 0xF591,
                                                            0x1794, 0x5C7B, 0xA7BF, 0x98E5, 0x0B63, 0xA1C9,
                                                            0x79BF, 0xF413, 0x82B1, 0xA895};
    const bytes::Bytes seed{0xde, 0xad, 0xbe, 0xef};
    return SsmProtocol::calculateSeedKey(seed, index, SsmProtocol::kIndexTransformationStock);
}

// The five legacy connect_bootloader() exchanges, byte-exact. Task 3 reuses
// this helper for the success-path ROM-read test.
void scriptConnect(ScriptedKlineFlashTransport& transport)
{
    const auto section = transport.section("connect");
    transport.exchange(frame({0xbf}), idResponse());
    transport.exchange(frame({0x81}), bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xc1, 0});
    transport.exchange(frame({0x83, 0x00}), bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xc3, 0});
    transport.exchange(frame({0x27, 0x01}), seedResponse());
    bytes::Bytes key_request{0x27, 0x02};
    const bytes::Bytes key = expectedSeedKey();
    key_request.insert(key_request.end(), key.begin(), key.end());
    transport.exchange(frame(key_request), bytes::Bytes{0x80, 0xf0, 0x18, 0x02, 0x67, 0x02, 0});
}

FlashPlan readPlan()
{
    auto plan = build_subaru_tcu_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_tcu_hitachi_m32r_kline",
                                                         "M32R_512KB", std::nullopt);
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

TEST(SubaruTcuHitachiM32rKlineExecutor, TransportSetupMatchesTheLegacySetters)
{
    SubaruTcuHitachiM32rKlineExecutor executor;
    const auto config = executor.transport_setup(readPlan());
    ASSERT_THAT(config, fastecu::testing::IsOk());
    EXPECT_EQ(config->baud, 4800);
    // set_is_iso14230_connection(true) in the legacy execute().
    EXPECT_TRUE(config->iso14230);
    EXPECT_EQ(config->tester_id, 0xf0);
    EXPECT_EQ(config->target_id, 0x18);
}

// read_rom is a Task-3 stub (returns ErrorKind::Internal unconditionally), so
// this only proves the five connect exchanges are sent byte-exact and in
// order, then that execute() surfaces the stub's failure and stops -- no
// further writes happen. The success-path version, scripting all 5462 ROM
// read chunks through to a passing result, arrives in Task 3 once read_rom is
// implemented.
TEST(SubaruTcuHitachiM32rKlineExecutor, ConnectSendsTheFiveLegacyExchangesInOrder)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::Internal));
    EXPECT_EQ(transport.writesConsumed(), 5u);
}
} // namespace
