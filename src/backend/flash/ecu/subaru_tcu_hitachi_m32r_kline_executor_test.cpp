#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

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

// Matches the executor's private kRomSize/block_size: 0x80000 / 96 = 5461
// remainder 32, so the ROM read is 5462 blocks with a 32-byte tail.
constexpr std::uint32_t kRomSize = 0x80000;
constexpr std::uint32_t kBlockSize = 96;

bytes::Bytes frame(bytes::Bytes payload)
{
    return SsmProtocol::addHeader(payload, 0xf0, 0x18);
}

// A scripted a0 block-read response: 5 header bytes, `length` data bytes
// (filled with `fill`), 1 checksum byte -- the shape read_rom slices as
// [5, size-1).
bytes::Bytes blockResponse(std::uint32_t length, bytes::Byte fill)
{
    bytes::Bytes response(length + 6, fill);
    response[4] = 0xe0;
    return response;
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

// Proves the five connect exchanges are sent byte-exact and in order, then
// that the real read_rom() reads all 5462 blocks through to a passing result.
TEST(SubaruTcuHitachiM32rKlineExecutor, ConnectSendsTheFiveLegacyExchangesInOrder)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    {
        const auto section = transport.section("read chunks");
        for (std::uint32_t address = 0; address < kRomSize; address += kBlockSize)
        {
            const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
            transport.exchange(
                frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                       static_cast<bytes::Byte>(address), static_cast<bytes::Byte>(length - 1)}),
                blockResponse(length, 0x5a));
        }
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    EXPECT_EQ(result->rom_id, std::string("123456789A_"));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(SubaruTcuHitachiM32rKlineExecutor, ReadsTheRomIn96ByteBlocksWithA32ByteTail)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    std::uint32_t blocks = 0;
    std::uint32_t last_length = 0;
    {
        const auto section = transport.section("read chunks");
        for (std::uint32_t address = 0; address < kRomSize; address += kBlockSize)
        {
            last_length = std::min(kBlockSize, kRomSize - address);
            ++blocks;
            transport.exchange(
                frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                       static_cast<bytes::Byte>(address), static_cast<bytes::Byte>(last_length - 1)}),
                blockResponse(last_length, 0x5a));
        }
    }
    EXPECT_EQ(blocks, 5462U);
    EXPECT_EQ(last_length, 32U);

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->read_bytes.has_value());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    EXPECT_TRUE(transport.scriptConsumed());
}

// The legacy retried a block up to five times while the response was 5 bytes
// or shorter; the sixth failure gave up silently and produced a short ROM.
TEST(SubaruTcuHitachiM32rKlineExecutor, RetriesABlockUpToFiveTimes)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    const bytes::Bytes request = frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f});
    // Four short responses, then a good one: the fifth attempt succeeds.
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        transport.exchange(request, bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xe0});
    }
    transport.exchange(request, blockResponse(kBlockSize, 0x5a));
    for (std::uint32_t address = kBlockSize; address < kRomSize; address += kBlockSize)
    {
        const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
        transport.exchange(
            frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                   static_cast<bytes::Byte>(address), static_cast<bytes::Byte>(length - 1)}),
            blockResponse(length, 0x5a));
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
}

// Deliberate divergence: the legacy appended nothing and returned
// STATUS_SUCCESS, yielding a silently short ROM.
TEST(SubaruTcuHitachiM32rKlineExecutor, FailsWhenABlockExhaustsItsFiveAttempts)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    const bytes::Bytes request = frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f});
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        transport.exchange(request, bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xe0});
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
}

// Deliberate divergence: the legacy accepted any response longer than five
// bytes and appended length-1 of it, misaligning every later byte.
TEST(SubaruTcuHitachiM32rKlineExecutor, RejectsABlockResponseOfTheWrongLength)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    transport.exchange(frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f}), blockResponse(kBlockSize - 8, 0x5a));

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
}
} // namespace
