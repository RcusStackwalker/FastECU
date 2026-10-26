#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{

using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;
using ::testing::StrEq;

TEST(DensoBeefCanCommonTest, BeefRequestFramesOpcodeAndPayloadLength)
{
    const bytes::Bytes payload{0x01, 0x02, 0x03};
    const bytes::Bytes framed = BeefRequest(0xB6, payload);

    ASSERT_EQ(framed.size(), 8U);
    EXPECT_EQ(framed[0], 0xBE);
    EXPECT_EQ(framed[1], 0xEF);
    EXPECT_EQ(framed[2], 0x00);
    EXPECT_EQ(framed[3], 0x04);
    EXPECT_EQ(framed[4], 0xB6);
    EXPECT_EQ(framed[5], 0x01);
    EXPECT_EQ(framed[6], 0x02);
    EXPECT_EQ(framed[7], 0x03);
}

TEST(DensoBeefCanCommonTest, BeefRequestWithNoPayloadStillCountsTheOpcode)
{
    const bytes::Bytes framed = BeefRequest(0xA0);

    ASSERT_EQ(framed.size(), 5U);
    EXPECT_EQ(framed[3], 0x01);
    EXPECT_EQ(framed[4], 0xA0);
}

TEST(DensoBeefCanCommonTest, ElapsedMillisecondsReportsAtLeastOne)
{
    const std::chrono::steady_clock::time_point start{};

    EXPECT_EQ(ElapsedMilliseconds(start, start), 1U);
    EXPECT_EQ(ElapsedMilliseconds(start, start + 250ms), 250U);
}

TEST(DensoBeefCanCommonTest, CancelledIfRequestedReportsTheCallerDetail)
{
    struct Ctx
    {
        const ICancellationToken& cancellation;
    };
    FakeCancellationToken token;
    const Ctx ctx{token};

    EXPECT_THAT(CancelledIfRequested(ctx, "cancelled before CAN request"), IsOk());

    token.SetCancelled(true);
    const Status cancelled = CancelledIfRequested(ctx, "cancelled before CAN request");
    EXPECT_THAT(cancelled, IsErrWith(ErrorKind::kCancelled, StrEq("cancelled before CAN request")));
}

TEST(DensoBeefCanCommonTest, ParseBeefBoundsEveryDeclaredField)
{
    const auto parse = [](bytes::Bytes pdu) { return ParseBeef(pdu); };
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00}), IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response envelope")));
    EXPECT_THAT(parse({0x12, 0x34, 0x00, 0x01, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("wrong BEEF response marker")));
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00, 0x09, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("invalid BEEF response length")));
    EXPECT_THAT(parse({0xBE, 0xEF, 0x00, 0x00, 0x00}),
                IsErrWith(ErrorKind::kBadResponse, StrEq("invalid BEEF response length")));
    for (const auto& [pdu, payload_size] : {std::pair{bytes::Bytes{0xBE, 0xEF, 0x00, 0x01, 0xB6}, 0U},
                                            std::pair{bytes::Bytes{0xBE, 0xEF, 0x00, 0x02, 0xB6, 0xAA, 0xFF}, 1U}})
    {
        SCOPED_TRACE(::testing::PrintToString(pdu));
        const auto parsed = ParseBeef(pdu);
        ASSERT_THAT(parsed, IsOk());
        EXPECT_EQ(parsed->opcode, 0xB6);
        EXPECT_EQ(parsed->payload.size(), payload_size);
    }
}

// The context shape the shared helpers expect: cancellation, events and clock.
struct CompareFixture
{
    FakeCancellationToken cancellation;
    RecordingEventSink sink;
    FakeClock fake_clock;
    IEventSink& events{sink};
    IClock& clock{fake_clock};
};

// Three 4-byte regions of an image that starts at ECU address 0x1000.
constexpr std::uint32_t kImageBase = 0x1000;
constexpr std::array<MemoryRegion, 3> kRegions{{{0x1000, 4}, {0x1004, 4}, {0x1008, 4}}};

TEST(DensoBeefCanCommonTest, CompareBeefBlocksChecksEachRegionAgainstItsOwnSliceOfTheImage)
{
    CompareFixture fixture;
    bytes::Bytes image(12);
    std::iota(image.begin(), image.end(), bytes::Byte{1});
    std::size_t queried = 0;

    const auto result =
        CompareBeefBlocks(fixture, kRegions, kImageBase, image, nullptr, false, "petrol",
                          [&](const MemoryRegion& block) -> Result<std::uint32_t>
                          {
                              const std::uint32_t crc = fastecu::checksum::Crc32(
                                  bytes::ByteView{image}.subspan(block.start - kImageBase, block.length));
                              return queried++ == 1 ? crc ^ 1U : crc;
                          });

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(queried, kRegions.size());
    EXPECT_EQ(result->changed_count, 1U);
    EXPECT_EQ(result->modified, (std::vector<bool>{false, true, false}));
    EXPECT_EQ(fixture.fake_clock.Elapsed(), 5ms * queried);
    EXPECT_EQ(fixture.sink.logs.at(fixture.sink.logs.size() - 2).second, "1, ");
}

TEST(DensoBeefCanCommonTest, CompareBeefBlocksRejectsRegionsOutsideTheImageAndNamesTheFamily)
{
    const auto never_asked = [](const MemoryRegion&) -> Result<std::uint32_t> { return 0U; };
    const bytes::Bytes image(12);
    CompareFixture fixture;

    EXPECT_THAT(CompareBeefBlocks(fixture, kRegions, kImageBase, bytes::ByteView{image}.first(11), nullptr, true,
                                  "petrol", never_asked),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("petrol CRC block is outside the image")));
    EXPECT_THAT(CompareBeefBlocks(fixture, kRegions, kImageBase + 1, image, nullptr, true, "petrol", never_asked),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("petrol CRC block is outside the image")));
    EXPECT_THAT(CompareBeefBlocks(fixture, kRegions, kImageBase, image, nullptr, true, "petrol", never_asked), IsOk());
}

TEST(DensoBeefCanCommonTest, CompareBeefBlocksStopsOnQueryFailureAndOnCancellationDuringTheSettle)
{
    const bytes::Bytes image(12);
    CompareFixture failing;
    EXPECT_THAT(CompareBeefBlocks(failing, kRegions, kImageBase, image, nullptr, false, "petrol",
                                  [](const MemoryRegion&) -> Result<std::uint32_t>
                                  { return Fail(ErrorKind::kTimeout, "no CRC reply"); }),
                IsErrWith(ErrorKind::kTimeout, StrEq("no CRC reply")));

    CompareFixture cancelling;
    EXPECT_THAT(CompareBeefBlocks(cancelling, kRegions, kImageBase, image, nullptr, false, "petrol",
                                  [&](const MemoryRegion&) -> Result<std::uint32_t>
                                  {
                                      cancelling.cancellation.SetCancelled(true);
                                      return 0U;
                                  }),
                IsErr(ErrorKind::kCancelled));
}

// A scripted channel behind the context shape the shared exchange helpers
// expect.
struct ExchangeFixture
{
    struct Channel
    {
        Status Send(bytes::ByteView, const ICancellationToken&)
        {
            return send_result;
        }
        Result<std::optional<bytes::Bytes>> Receive(std::chrono::milliseconds, const ICancellationToken&)
        {
            return reply;
        }
        Status send_result;
        Result<std::optional<bytes::Bytes>> reply{std::nullopt};
    };
    Channel channel;
    FakeClock clock;
    FakeCancellationToken cancellation;
};

TEST(DensoBeefCanCommonTest, BeefExchangeReturnsThePayloadOfAnOpcodeOrSizeCheckedReply)
{
    ExchangeFixture fixture;
    const auto exchange = [&](bytes::Bytes reply, std::size_t minimum)
    {
        fixture.channel.reply = std::move(reply);
        return BeefExchange(fixture, 0x05, {}, minimum, 10ms);
    };
    const bytes::Bytes good{0xBE, 0xEF, 0x00, 0x03, 0x45, 0xAA, 0xBB};

    EXPECT_THAT(exchange(good, 2), IsOk());
    EXPECT_THAT(exchange(good, 3), IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response payload")));
    EXPECT_THAT(exchange({0xBE, 0xEF, 0x00, 0x01, 0x46}, 0),
                IsErrWith(ErrorKind::kBadResponse, StrEq("unexpected BEEF response opcode 0x46, expected 0x45")));
    EXPECT_THAT(exchange({0xBE}, 0), IsErrWith(ErrorKind::kBadResponse, StrEq("short BEEF response envelope")));

    fixture.channel.reply = std::nullopt;
    EXPECT_THAT(BeefExchange(fixture, 0x05, {}, 0, 10ms),
                IsErrWith(ErrorKind::kTimeout, StrEq("no CAN response within the read timeout")));
    fixture.channel.reply = Fail(ErrorKind::kDisconnected, "gone");
    EXPECT_THAT(BeefExchange(fixture, 0x05, {}, 0, 10ms), IsErr(ErrorKind::kDisconnected));
}

TEST(DensoBeefCanCommonTest, InitializeBeefFlashSelectsTheModeAndStopsAtTheFirstFailingExchange)
{
    for (const bool test_write : {false, true})
    {
        SCOPED_TRACE(::testing::PrintToString(test_write));
        CompareFixture fixture;
        std::vector<bytes::Byte> opcodes;
        EXPECT_THAT(InitializeBeefFlash(fixture, test_write, test_write,
                                        [&](bytes::Byte opcode, std::size_t) -> Result<bytes::Bytes>
                                        {
                                            opcodes.push_back(opcode);
                                            return bytes::Bytes{0x00, 0x00, 0xAB, 0xCD};
                                        }),
                    IsOk());
        EXPECT_EQ(opcodes, (std::vector<bytes::Byte>{0x05, 0x06, test_write ? bytes::Byte{0x21} : bytes::Byte{0x20}}));
        EXPECT_EQ(fixture.sink.logs[1].second, test_write ? ": 0xABCD" : ": 0xabcd");
    }
    for (const std::size_t failing : {0U, 1U, 2U})
    {
        SCOPED_TRACE(::testing::PrintToString(failing));
        CompareFixture fixture;
        std::size_t calls = 0;
        EXPECT_THAT(InitializeBeefFlash(fixture, false, false,
                                        [&](bytes::Byte, std::size_t) -> Result<bytes::Bytes>
                                        {
                                            return calls++ == failing ? Fail(ErrorKind::kTimeout, "no reply")
                                                                      : Result<bytes::Bytes>{bytes::Bytes(4)};
                                        }),
                    IsErrWith(ErrorKind::kTimeout, StrEq("no reply")));
        EXPECT_EQ(calls, failing + 1);
    }
}

// A BEEF exchange that records every opcode and fails on the `failing`-th call
// (counting from 0), calling `on_call` first.
struct ScriptedExchange
{
    Result<bytes::Bytes> operator()(bytes::Byte opcode, bytes::ByteView, std::size_t minimum, std::chrono::milliseconds)
    {
        opcodes.push_back(opcode);
        if (on_call)
        {
            on_call(opcodes.size() - 1);
        }
        if (opcodes.size() - 1 == failing)
        {
            return Fail(ErrorKind::kTimeout, "no reply");
        }
        return bytes::Bytes(std::max<std::size_t>(minimum, 2), bytes::Byte{0xAB});
    }

    std::size_t failing = std::numeric_limits<std::size_t>::max();
    std::function<void(std::size_t)> on_call;
    std::vector<bytes::Byte> opcodes;
};

TEST(DensoBeefCanCommonTest, ReadBeefMemoryTruncatesTheLastPageAndStopsOnFailureOrCancellation)
{
    const MemoryRegion region{0x1000, 0x500};
    CompareFixture fixture;
    PhaseReporter progress(fixture.events, "Read", 1, 1, 0x500);
    ScriptedExchange exchange;

    const auto rom = ReadBeefMemory(fixture, region, progress, std::ref(exchange));

    ASSERT_THAT(rom, IsOk());
    EXPECT_THAT(*rom, ::testing::AllOf(::testing::SizeIs(0x500), test_bytes::BytesFilledWith(bytes::Byte{0xAB})));
    EXPECT_EQ(exchange.opcodes, (std::vector<bytes::Byte>{0x03, 0x03}));

    ScriptedExchange failing{.failing = 1};
    EXPECT_THAT(ReadBeefMemory(fixture, region, progress, std::ref(failing)), IsErr(ErrorKind::kTimeout));
    fixture.cancellation.SetCancelled(true);
    EXPECT_THAT(ReadBeefMemory(fixture, region, progress, std::ref(exchange)), IsErr(ErrorKind::kCancelled));
}

TEST(DensoBeefCanCommonTest, FlashBeefBlockRejectsBlocksThatDoNotFitTheImageAndNamesTheFamily)
{
    CompareFixture fixture;
    PhaseReporter progress(fixture.events, "Write", 1, 1, 0x1000);
    const bytes::Bytes image(0x1000);
    std::size_t index = 0;
    const auto flash = [&](const MemoryRegion& block, std::uint32_t base)
    {
        ScriptedExchange exchange;
        return FlashBeefBlock(fixture, image, base, block, false, index, 0x1000, progress, "diesel",
                              std::ref(exchange));
    };

    EXPECT_THAT(flash({0x2000, 0x300}, 0x2000),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("diesel flash block is not aligned to write windows")));
    EXPECT_THAT(flash({0x2000, 0}, 0x2000),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("diesel flash block is not aligned to write windows")));
    EXPECT_THAT(flash({0x2000, 0x1000}, 0x2001),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("diesel flash block starts before the image")));
    EXPECT_THAT(flash({0x2000, 0x1000}, 0x1001),
                IsErrWith(ErrorKind::kInvalidConfig, StrEq("diesel flash block extends beyond the image")));
}

TEST(DensoBeefCanCommonTest, FlashBeefBlockErasesWritesAndCommitsOrValidatesOneWindow)
{
    for (const bool test_write : {false, true})
    {
        SCOPED_TRACE(::testing::PrintToString(test_write));
        CompareFixture fixture;
        PhaseReporter progress(fixture.events, "Write", 1, 1, 0x1000);
        ScriptedExchange exchange;
        std::size_t index = 0;

        EXPECT_THAT(FlashBeefBlock(fixture, bytes::Bytes(0x1000), 0x2000, {0x2000, 0x1000}, test_write, index, 0x1000,
                                   progress, "petrol", std::ref(exchange)),
                    IsOk());
        EXPECT_EQ(index, 0x1000U);
        EXPECT_EQ(exchange.opcodes.size(), 10U);
        EXPECT_EQ(exchange.opcodes.front(), 0x25);
        EXPECT_EQ(exchange.opcodes.back(), test_write ? 0x23 : 0x24);
    }
    for (const std::size_t failing : {0U, 1U, 9U})
    {
        SCOPED_TRACE(::testing::PrintToString(failing));
        CompareFixture fixture;
        PhaseReporter progress(fixture.events, "Write", 1, 1, 0x1000);
        ScriptedExchange exchange{.failing = failing};
        std::size_t index = 0;
        EXPECT_THAT(FlashBeefBlock(fixture, bytes::Bytes(0x1000), 0x2000, {0x2000, 0x1000}, false, index, 0x1000,
                                   progress, "petrol", std::ref(exchange)),
                    IsErr(ErrorKind::kTimeout));
    }
    for (const std::size_t cancel_at : {0U, 8U})
    {
        SCOPED_TRACE(::testing::PrintToString(cancel_at));
        CompareFixture fixture;
        PhaseReporter progress(fixture.events, "Write", 1, 1, 0x1000);
        ScriptedExchange exchange{.on_call = [&](std::size_t call)
                                  {
                                      if (call == cancel_at)
                                      {
                                          fixture.cancellation.SetCancelled(true);
                                      }
                                  }};
        std::size_t index = 0;
        EXPECT_THAT(FlashBeefBlock(fixture, bytes::Bytes(0x1000), 0x2000, {0x2000, 0x1000}, false, index, 0x1000,
                                   progress, "petrol", std::ref(exchange)),
                    IsErr(ErrorKind::kCancelled));
    }
}

TEST(DensoBeefCanCommonTest, WriteBeefMemoryLogsTheVoltageAndTheTimeLeftFromTheBytesStillToWrite)
{
    const std::array<MemoryRegion, 1> regions{{{0x2000, 0x1000}}};
    const bytes::Bytes image(0x1000);
    CompareFixture fixture;
    PhaseSequence phases(fixture.events, 4);
    PhaseReporter compare_progress = phases.Start("Compare", 1);
    const auto exchange = [](bytes::Byte opcode, bytes::ByteView, std::size_t,
                             std::chrono::milliseconds) -> Result<bytes::Bytes>
    { return opcode == 0x04 ? bytes::Bytes{0x00, 0x64} : bytes::Bytes(2); };
    const auto compare = [](PhaseReporter *, bool after) -> Result<CompareResult>
    { return CompareResult{.modified = {!after}, .changed_count = after ? 0U : 1U}; };

    EXPECT_THAT(WriteBeefMemory(fixture, image, 0x2000, regions, false, phases, compare_progress, "petrol", exchange,
                                compare, [](bool) { return Status{}; }),
                IsOk());
    const auto has_log = [&](std::string_view text)
    { return std::ranges::any_of(fixture.sink.logs, [&](const auto& log) { return log.second == text; }); };
    EXPECT_TRUE(has_log(": 2V"));
}

TEST(DensoBeefCanCommonTest, FlashBeefBlockReportsTheTimeLeftForTheRestOfTheWrite)
{
    CompareFixture fixture;
    PhaseReporter progress(fixture.events, "Write", 1, 1, 0x1000);
    ScriptedExchange exchange;
    std::size_t index = 0x200;

    EXPECT_THAT(FlashBeefBlock(fixture, bytes::Bytes(0x1000), 0x2000, {0x2000, 0x1000}, false, index,
                               0x400 + 5 * 512000 - 1, progress, "petrol", std::ref(exchange)),
                IsOk());
    EXPECT_EQ(fixture.sink.logs[5].second, "Write flash buffer: 0x00002000 (0% - 512000 B/s, ~ 5 s)");
}

TEST(DensoBeefCanCommonTest, SendChannelRequestSleepsOnlyForAPositiveDelay)
{
    struct Channel
    {
        Status Send(bytes::ByteView, const ICancellationToken&)
        {
            return {};
        }
    };
    struct Ctx
    {
        Channel channel;
        FakeClock clock;
        FakeCancellationToken cancellation;
    } ctx;
    ctx.clock.SetSleepAdvance(5ms);

    EXPECT_THAT(SendChannelRequest(ctx, bytes::Bytes{0x01}, 0ms), IsOk());
    EXPECT_EQ(ctx.clock.Elapsed(), 0ms);
    EXPECT_THAT(SendChannelRequest(ctx, bytes::Bytes{0x01}, 10ms), IsOk());
    EXPECT_EQ(ctx.clock.Elapsed(), 5ms);
}

TEST(DensoBeefCanCommonTest, UppercaseHexCompactHasNoSeparators)
{
    EXPECT_EQ(UppercaseHexCompact(bytes::Bytes{0x0A, 0xFF, 0x00}), "0AFF00");
}

} // namespace
} // namespace fastecu::flash
