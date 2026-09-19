#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
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

// The ROM byte the scripted TCU returns at `offset`. 31 is odd, so every
// byte in a 256-byte run is distinct from its neighbours: a block-response
// slice that moves by one at either end reconstructs different ROM content,
// not merely content of a different length.
bytes::Byte romByte(std::uint32_t offset)
{
    return static_cast<bytes::Byte>(0x11U + offset * 31U);
}

// The trailing checksum byte of a block response. Deliberately a value
// romByte() does not produce anywhere in the first or last block, so a slice
// that keeps the checksum is visible in the asserted ROM contents.
constexpr bytes::Byte kBlockChecksum = 0xc7;

// A scripted a0 block-read response for `length` bytes at `address`: 5
// header bytes, `length` data bytes, 1 checksum byte -- the shape read_rom
// slices as [5, size-1). Header, payload and checksum all carry distinct,
// recognizable values so the slice bounds are pinned by the ROM-content
// assertions below (see expectRomBlock) rather than only by its size.
bytes::Bytes blockResponse(std::uint32_t address, std::uint32_t length)
{
    bytes::Bytes response{0x80, 0xf0, 0x18, static_cast<bytes::Byte>(length + 1), 0xe0};
    for (std::uint32_t i = 0; i < length; ++i)
    {
        response.push_back(romByte(address + i));
    }
    response.push_back(kBlockChecksum);
    return response;
}

// Asserts the reconstructed ROM carries exactly the `length` bytes the TCU
// was scripted to return at `address`. Fails if read_rom's [5, size-1) slice
// moves by one at either end: a low bound of 4 leads with the 0xe0 header
// byte, and a high bound of size() trails with kBlockChecksum.
void expectRomBlock(const bytes::Bytes& rom, std::uint32_t address, std::uint32_t length)
{
    ASSERT_GE(rom.size(), address + length);
    bytes::Bytes expected;
    expected.reserve(length);
    for (std::uint32_t i = 0; i < length; ++i)
    {
        expected.push_back(romByte(address + i));
    }
    const auto begin = rom.begin() + static_cast<std::ptrdiff_t>(address);
    EXPECT_EQ(bytes::Bytes(begin, begin + static_cast<std::ptrdiff_t>(length)), expected);
}

// The scripted transport validates writes and reads against two independent
// queues (see ScriptedKlineFlashTransport::write()/read()) that are never
// cross-checked against each other's call order, and FakeClock's sleep is
// instantaneous and invisible to the transport. So nothing else in this
// suite -- not even a test that swaps the two calls or drops a sleep in
// exchange_block_read() -- would fail on wrong ordering: writesConsumed(),
// scriptConsumed(), and the ROM bytes would all still come out right. These
// two mocks trace the actual call order so PacesEachBlockReadAsWriteThen
// DelayThenReadThenDelay below can pin it: this family's 100ms gap between
// request and response is legacy wire timing on an unqualified 4800-baud
// K-Line path, and the point of the pacing code is to reproduce that timing
// exactly, not to be "improved".
enum class Step
{
    Write,
    Read,
    Sleep
};

// Not `final`: StopsPromptlyWhenCancelledMidRead below subclasses this to
// add a cancellation trip while keeping the same Write/Read tracing.
class TracingTransport : public ScriptedKlineFlashTransport
{
  public:
    TracingTransport(ScriptedTransportInitialState initial_state, std::vector<Step>& trace)
        : ScriptedKlineFlashTransport(initial_state), trace_(trace)
    {
    }
    Result<std::size_t> write(bytes::ByteView data) override
    {
        trace_.push_back(Step::Write);
        return ScriptedKlineFlashTransport::write(data);
    }
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        trace_.push_back(Step::Read);
        return ScriptedKlineFlashTransport::read(timeout, cancellation);
    }

  private:
    std::vector<Step>& trace_;
};

class TracingClock final : public FakeClock
{
  public:
    explicit TracingClock(std::vector<Step>& trace) : trace_(trace)
    {
    }
    Status sleep(std::chrono::milliseconds duration, const ICancellationToken& token) override
    {
        trace_.push_back(Step::Sleep);
        return FakeClock::sleep(duration, token);
    }

  private:
    std::vector<Step>& trace_;
};

// This family's connect_bootloader() consumes five transport.read() calls
// (id, 0x81, 0x83, seed request, key send) before the ROM read loop starts,
// so this trips on the eighth read: the five connect reads plus the first
// two block reads, landing the cancellation on the third block's read --
// squarely inside read_rom's loop rather than inside connect.
constexpr int kCancelOnRead = 8;

// Composes onto TracingTransport so StopsPromptlyWhenCancelledMidRead can
// assert the exact Write/Sleep/Read/Sleep trace, not just a bounded write
// count -- see the comment on that test for why the trace is what actually
// distinguishes the inner exchange_block_read checkpoint from read_rom's
// coarser per-iteration one.
class TripOnReadTracingTransport final : public TracingTransport
{
  public:
    TripOnReadTracingTransport(std::vector<Step>& trace, ManualCancellationToken& source)
        : TracingTransport(ScriptedTransportInitialState::Open, trace), source_(source)
    {
    }
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        auto result = TracingTransport::read(timeout, cancellation);
        if (++reads_ == kCancelOnRead)
        {
            source_.cancel();
        }
        return result;
    }

  private:
    ManualCancellationToken& source_;
    int reads_ = 0;
};

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
                blockResponse(address, length));
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
    // Pins the [5, size-1) slice at both ends: the first block's leading byte
    // and the tail block's trailing byte are what an off-by-one would eat.
    expectRomBlock(*result->read_bytes, 0, kBlockSize);
    expectRomBlock(*result->read_bytes, kRomSize - 32U, 32U);
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
                blockResponse(address, last_length));
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
    expectRomBlock(*result->read_bytes, 0, kBlockSize);
    expectRomBlock(*result->read_bytes, kRomSize - last_length, last_length);
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
    transport.exchange(request, blockResponse(0, kBlockSize));
    for (std::uint32_t address = kBlockSize; address < kRomSize; address += kBlockSize)
    {
        const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
        transport.exchange(
            frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                   static_cast<bytes::Byte>(address), static_cast<bytes::Byte>(length - 1)}),
            blockResponse(address, length));
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    expectRomBlock(*result->read_bytes, 0, kBlockSize);
}

// A read that returns no frame at all is what DesktopKlineFlashTransport
// hands back for an empty serial read, and it is the legacy
// `received.length() == 0` case: a spent attempt, not a fatal error. This
// pins that an empty read inside the block loop is retried rather than
// ending the whole 5462-block ROM read with Timeout.
TEST(SubaruTcuHitachiM32rKlineExecutor, RetriesABlockWhenAReadProducesNoFrame)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    const bytes::Bytes request = frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f});
    // Attempts 1, 2 and 4 return no frame and attempt 3 a too-short frame;
    // both kinds are spent attempts, so the fifth attempt still succeeds.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        transport.expectWrite(request);
        transport.queue_no_frame();
    }
    transport.exchange(request, bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xe0});
    transport.expectWrite(request);
    transport.queue_no_frame();
    transport.exchange(request, blockResponse(0, kBlockSize));
    for (std::uint32_t address = kBlockSize; address < kRomSize; address += kBlockSize)
    {
        const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
        transport.exchange(
            frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                   static_cast<bytes::Byte>(address), static_cast<bytes::Byte>(length - 1)}),
            blockResponse(address, length));
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(result->read_bytes->size(), kRomSize);
    expectRomBlock(*result->read_bytes, 0, kBlockSize);
    expectRomBlock(*result->read_bytes, kRomSize - 32U, 32U);
    EXPECT_TRUE(transport.scriptConsumed());
}

// Five consecutive no-frame reads exhaust the block's attempts and land on
// the same BadResponse path a run of too-short frames does -- never Timeout,
// which would mean the first empty read had aborted the whole ROM read.
TEST(SubaruTcuHitachiM32rKlineExecutor, FailsWhenFiveConsecutiveBlockReadsProduceNoFrame)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    scriptConnect(transport);
    const auto section = transport.section("read chunks");
    const bytes::Bytes request = frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f});
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        transport.expectWrite(request);
        transport.queue_no_frame();
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_TRUE(transport.scriptConsumed());
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
    transport.exchange(frame({0xa0, 0x00, 0x00, 0x00, 0x00, 0x5f}), blockResponse(0, kBlockSize - 8));

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::BadResponse));
}

// See the comment on Step/TracingTransport/TracingClock above for why this
// test exists: it is the only one in this suite that can distinguish the
// legacy write/delay(100)/read/delay(100) order from any other interleaving
// of the same four calls.
TEST(SubaruTcuHitachiM32rKlineExecutor, PacesEachBlockReadAsWriteThenDelayThenReadThenDelay)
{
    std::vector<Step> trace;
    TracingTransport transport{ScriptedTransportInitialState::Open, trace};
    scriptConnect(transport);
    {
        const auto section = transport.section("read chunks");
        for (std::uint32_t address = 0; address < kRomSize; address += kBlockSize)
        {
            const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
            transport.exchange(
                frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                       static_cast<bytes::Byte>(address), static_cast<bytes::Byte>(length - 1)}),
                blockResponse(address, length));
        }
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    TracingClock clock{trace};
    ManualCancellationToken cancellation;
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_EQ(trace.size(), 1U + 10U + 4U * 5462U);
    // Legacy connect_bootloader() opens with delay(100), before any traffic.
    EXPECT_EQ(trace[0], Step::Sleep);
    // Connect: five plain exchange() round trips -- Write/Read, no Sleep.
    for (std::size_t i = 1; i < 11U; i += 2)
    {
        EXPECT_EQ(trace[i], Step::Write);
        EXPECT_EQ(trace[i + 1], Step::Read);
    }
    // First block read: Write, Sleep, Read, Sleep -- exactly the legacy
    // send_sid_a0_block_read() order, with the gap between write and read.
    EXPECT_EQ(trace[11], Step::Write);
    EXPECT_EQ(trace[12], Step::Sleep);
    EXPECT_EQ(trace[13], Step::Read);
    EXPECT_EQ(trace[14], Step::Sleep);
}

// Proves the *inner* "cancelled after read" checkpoint in
// exchange_block_read is what stops the loop, not just read_rom's coarser
// per-iteration checkpoint. A bounded write count alone can't distinguish
// the two: whichever checkpoint catches it, block 4 never writes, so both
// paths would end at 8 writes. The traced step sequence can, though: the
// inner checkpoint returns immediately after the read, before the block's
// trailing sleep runs, so the block that lands the cancellation contributes
// only Write, Sleep, Read (3 steps) instead of the usual Write, Sleep,
// Read, Sleep (4). If only the outer per-iteration checkpoint existed, that
// block would complete its trailing sleep and the trace would be one step
// longer, with trace[20] a Sleep instead of the last Read.
TEST(SubaruTcuHitachiM32rKlineExecutor, StopsPromptlyWhenCancelledMidRead)
{
    std::vector<Step> trace;
    ManualCancellationToken cancellation;
    TripOnReadTracingTransport transport{trace, cancellation};
    scriptConnect(transport);
    {
        const auto section = transport.section("read chunks");
        for (std::uint32_t address = 0; address < kRomSize; address += kBlockSize)
        {
            const std::uint32_t length = std::min(kBlockSize, kRomSize - address);
            transport.exchange(
                frame({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                       static_cast<bytes::Byte>(address), static_cast<bytes::Byte>(length - 1)}),
                blockResponse(address, length));
        }
    }

    SubaruTcuHitachiM32rKlineExecutor executor;
    TracingClock clock{trace};
    RecordingEventSink events;
    const auto result = executor.execute(readPlan(), transport, clock, cancellation, events);

    EXPECT_THAT(result, fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_LT(transport.writesConsumed(), 100U);

    ASSERT_EQ(trace.size(), 22U);
    // Legacy connect_bootloader() opens with delay(100), before any traffic.
    EXPECT_EQ(trace[0], Step::Sleep);
    // Connect: five plain exchange() round trips -- Write/Read, no Sleep.
    for (std::size_t i = 1; i < 11U; i += 2)
    {
        EXPECT_EQ(trace[i], Step::Write);
        EXPECT_EQ(trace[i + 1], Step::Read);
    }
    // Blocks 1 and 2 complete in full: Write, Sleep, Read, Sleep each.
    EXPECT_EQ(trace[11], Step::Write);
    EXPECT_EQ(trace[12], Step::Sleep);
    EXPECT_EQ(trace[13], Step::Read);
    EXPECT_EQ(trace[14], Step::Sleep);
    EXPECT_EQ(trace[15], Step::Write);
    EXPECT_EQ(trace[16], Step::Sleep);
    EXPECT_EQ(trace[17], Step::Read);
    EXPECT_EQ(trace[18], Step::Sleep);
    // Block 3 lands the cancellation on its read (the 8th overall) and
    // stops there -- no trailing Sleep.
    EXPECT_EQ(trace[19], Step::Write);
    EXPECT_EQ(trace[20], Step::Sleep);
    EXPECT_EQ(trace[21], Step::Read);
}

// A plan built for the ECU (non-TCU) Hitachi M32R K-Line family must be
// rejected by check_family before any I/O happens.
TEST(SubaruTcuHitachiM32rKlineExecutor, RejectsAPlanBuiltForAnotherFamily)
{
    auto foreign = build_subaru_hitachi_m32r_kline_plan(FlashOperation::Read, "sub_ecu_hitachi_m32r_kline",
                                                        "M32R_512KB_1block", std::nullopt);
    ASSERT_THAT(foreign, fastecu::testing::IsOk());

    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    EXPECT_THAT(executor.transport_setup(*foreign), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(executor.execute(*foreign, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruTcuHitachiM32rKlineExecutor, FailsWhenTheSeedResponseIsTooShort)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    const auto section = transport.section("connect");
    transport.exchange(frame({0xbf}), idResponse());
    transport.exchange(frame({0x81}), bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xc1, 0});
    transport.exchange(frame({0x83, 0x00}), bytes::Bytes{0x80, 0xf0, 0x18, 0x01, 0xc3, 0});
    // 0x67 0x01 present but only two seed bytes follow.
    transport.exchange(frame({0x27, 0x01}), bytes::Bytes{0x80, 0xf0, 0x18, 0x04, 0x67, 0x01, 0xde, 0xad, 0});

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    EXPECT_THAT(executor.execute(readPlan(), transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::BadResponse));
}

TEST(SubaruTcuHitachiM32rKlineExecutor, FailsWhenTheTcuNeverAnswers)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};
    transport.expectWrite(frame({0xbf}));
    transport.queue_no_frame();

    SubaruTcuHitachiM32rKlineExecutor executor;
    FakeClock clock;
    ManualCancellationToken cancellation;
    RecordingEventSink events;

    EXPECT_THAT(executor.execute(readPlan(), transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::Timeout));
}
} // namespace
