#include "src/algorithms/protocol/testing/byte_matchers.h"
#include <array>
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <iterator>
#include <ranges>
#include <string_view>
#include <tuple>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
#include "src/backend/flash/testing/faulting_kline_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
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
using Line = ScriptedKlineFlashTransport::ControlLineAction;
using ::testing::_;
using ::testing::Contains;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Optional;
using ::testing::Pair;

constexpr std::string_view kProtocol = "sub_ecu_unisia_jecs_20";
constexpr std::string_view kMcu = "M32R_128KB";
constexpr std::uint32_t kRomSize = 0x20000;
constexpr std::string_view kEcuId = "123456789A";

// Built independently of SsmProtocol so the tests pin the wire bytes.
bytes::Bytes Request(const bytes::Bytes& payload)
{
    bytes::Bytes frame{0x80, 0x10, 0xf0, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(bytes::Sum8(frame));
    return frame;
}

bytes::Bytes Reply(const bytes::Bytes& payload)
{
    bytes::Bytes frame{0x80, 0xf0, 0x10, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(bytes::Sum8(frame));
    return frame;
}

// FF, three capability bytes, the five ECU ID bytes at frame offset 8, then
// trailing capability bytes legacy discarded (read_mem()).
bytes::Bytes InitReply()
{
    return Reply({0xff, 0xa1, 0xa2, 0xa3, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xb1, 0xb2});
}

bytes::Bytes PageData(std::uint32_t page)
{
    return bytes::Bytes(0x80, static_cast<bytes::Byte>(page * 7 + 1));
}

bytes::Bytes PageRequest(std::uint32_t address)
{
    return Request({0xa0, 0x00, static_cast<bytes::Byte>(address >> 16U), static_cast<bytes::Byte>(address >> 8U),
                    static_cast<bytes::Byte>(address), 0x7f});
}

bytes::Bytes PageReply(std::uint32_t page)
{
    bytes::Bytes payload{0xe0};
    const bytes::Bytes data = PageData(page);
    payload.insert(payload.end(), data.begin(), data.end());
    return Reply(payload);
}

void ScriptWarmEntry(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.Section("read-mode probe answered");
    transport.Exchange(Request({0xbf}), InitReply());
}

void ScriptColdEntry(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.Section("cold init");
    transport.ExpectWrite(Request({0xbf}));
    transport.QueueNoFrame();
    transport.Exchange(Request({0xbf}), InitReply());
    transport.Exchange(Request({0xb8, 0x00, 0x00, 0x00, 0x75}), Reply({0xf8, 0x75}));
    transport.Exchange(Request({0xbf}), InitReply());
}

void ScriptPages(ScriptedKlineFlashTransport& transport, std::uint32_t count)
{
    auto section = transport.Section("A0 pages");
    for (std::uint32_t page = 0; page < count; ++page)
    {
        transport.Exchange(PageRequest(0x100000 + page * 0x80), PageReply(page));
    }
}

Result<FlashPlan> ReadPlan(std::string_view protocol = kProtocol, std::string_view mcu = kMcu)
{
    return BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kRead, protocol, mcu, std::nullopt, false);
}

struct RunContext
{
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
};

Result<FlashExecutionResult> RunScripted(const FlashPlan& plan, ScriptedKlineFlashTransport& transport,
                                         RunContext& context)
{
    return SubaruUnisiaJecsM32rKlineExecutor{}.Execute(plan, transport, context.clock, context.cancellation,
                                                       context.events);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TransportSetupIs4800BaudPlainSsm)
{
    auto built_plan_1 = ReadPlan();
    ASSERT_THAT(built_plan_1, fastecu::testing::IsOk());
    const auto setup = SubaruUnisiaJecsM32rKlineExecutor{}.TransportSetup(std::move(*built_plan_1));
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 4800);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0xf0);
    EXPECT_EQ(setup->target_id, 0x10);
    EXPECT_EQ(setup->parity, KlineParity::kNone);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BeforeConfigureClearsTheIso14230Header)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruUnisiaJecsM32rKlineExecutor{}.BeforeTransportConfigure(transport, clock, cancellation), IsOk());
    EXPECT_EQ(transport.header_mode_calls, std::vector<bool>{false});
    EXPECT_THAT(transport.lifecycle_calls, IsEmpty());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ReadsTheRomWhenAlreadyInReadMode)
{
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    ScriptPages(transport, kRomSize / 0x80);
    RunContext context;

    auto built_plan_2 = ReadPlan();
    ASSERT_THAT(built_plan_2, fastecu::testing::IsOk());
    auto result = RunScripted(std::move(*built_plan_2), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kRead);
    ASSERT_THAT(result->read_bytes, Optional(_));
    ASSERT_EQ(result->read_bytes->size(), kRomSize);
    for (std::uint32_t page = 0; page < kRomSize / 0x80; ++page)
    {
        SCOPED_TRACE(::testing::PrintToString(page));
        const auto offset = static_cast<std::ptrdiff_t>(page) * 0x80;
        ASSERT_THAT(bytes::ByteView(*result->read_bytes).subspan(static_cast<std::size_t>(offset), 0x80),
                    test_bytes::BytesEq(PageData(page)))
            << page;
    }
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, std::vector<int>{38400});
    EXPECT_THAT(transport.control_line_trace, IsEmpty()) << "legacy read touched no LEC line";
    EXPECT_EQ(transport.read_timeouts.front(), 2000ms);
    EXPECT_EQ(transport.read_timeouts.back(), 3000ms);
    EXPECT_EQ(context.events.progress_calls.back(), (std::pair<int, int>{1024, 1024}));
    EXPECT_EQ(context.clock.Elapsed(), 1024 * 1ms);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitSwitchesBaudBeforeReading)
{
    ScriptedKlineFlashTransport transport;
    ScriptColdEntry(transport);
    ScriptPages(transport, kRomSize / 0x80);
    RunContext context;

    auto built_plan_3 = ReadPlan();
    ASSERT_THAT(built_plan_3, fastecu::testing::IsOk());
    auto result = RunScripted(std::move(*built_plan_3), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{38400, 4800, 38400}));
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TruncatedProbeReplyFallsBackToColdInit)
{
    ScriptedKlineFlashTransport transport;
    // A valid FF frame too short to carry the five ID bytes.
    transport.Exchange(Request({0xbf}), Reply({0xff, 0xa1}));
    transport.Exchange(Request({0xbf}), InitReply());
    transport.Exchange(Request({0xb8, 0x00, 0x00, 0x00, 0x75}), Reply({0xf8, 0x75}));
    transport.Exchange(Request({0xbf}), InitReply());
    ScriptPages(transport, kRomSize / 0x80);
    RunContext context;

    auto built_plan_4 = ReadPlan();
    ASSERT_THAT(built_plan_4, fastecu::testing::IsOk());
    auto result = RunScripted(std::move(*built_plan_4), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->rom_id, std::optional<std::string>(std::string(kEcuId) + "_"));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitRejectsABadBaudChangeReply)
{
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xbf}));
    transport.QueueNoFrame();
    transport.Exchange(Request({0xbf}), InitReply());
    transport.Exchange(Request({0xb8, 0x00, 0x00, 0x00, 0x75}), Reply({0x7f, 0xb8, 0x22}));
    RunContext context;

    auto built_plan_5 = ReadPlan();
    ASSERT_THAT(built_plan_5, fastecu::testing::IsOk());
    auto result = RunScripted(std::move(*built_plan_5), transport, context);

    EXPECT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 3U);
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{38400, 4800}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitWithoutAnyReplyTimesOut)
{
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xbf}));
    transport.QueueNoFrame();
    transport.ExpectWrite(Request({0xbf}));
    transport.QueueNoFrame();
    RunContext context;

    auto built_plan_6 = ReadPlan();
    ASSERT_THAT(built_plan_6, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_6), transport, context), IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ColdInitReplyTooShortForTheEcuIdFails)
{
    // expect_ssm_init() gates the cold BF reply on carrying the full five-byte
    // ECU ID; a valid FF frame that is too short is rejected rather than
    // accepted with whatever bytes were present (read_mem()).
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xbf}));
    transport.QueueNoFrame();
    transport.Exchange(Request({0xbf}), Reply({0xff, 0xa1}));
    RunContext context;

    auto built_plan_7 = ReadPlan();
    ASSERT_THAT(built_plan_7, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_7), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ReadFailsWhenTheInitialBaudChangeFails)
{
    ScriptedKlineFlashTransport transport;
    transport.set_baud_result = Fail(ErrorKind::kDisconnected, "adapter gone");
    RunContext context;

    auto built_plan_8 = ReadPlan();
    ASSERT_THAT(built_plan_8, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_8), transport, context), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, ShortPageFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    bytes::Bytes short_page{0xe0};
    short_page.resize(0x80, 0x11); // SID + 127 data bytes
    transport.Exchange(PageRequest(0x100000), Reply(short_page));
    RunContext context;

    auto built_plan_9 = ReadPlan();
    ASSERT_THAT(built_plan_9, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_9), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BadChecksumPageFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    bytes::Bytes corrupted = PageReply(0);
    corrupted.back() ^= 0x01U;
    transport.Exchange(PageRequest(0x100000), corrupted);
    RunContext context;

    auto built_plan_10 = ReadPlan();
    ASSERT_THAT(built_plan_10, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_10), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, NegativePageReplyFailsBeforeTheNextRequest)
{
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    transport.Exchange(PageRequest(0x100000), Reply({0x7f, 0xa0, 0x31}));
    RunContext context;

    auto built_plan_11 = ReadPlan();
    ASSERT_THAT(built_plan_11, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_11), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, MissingPageTimesOut)
{
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    transport.ExpectWrite(PageRequest(0x100000));
    transport.QueueNoFrame();
    RunContext context;

    auto built_plan_12 = ReadPlan();
    ASSERT_THAT(built_plan_12, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_12), transport, context), IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationStopsTheReadBeforeTheNextPage)
{
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    ScriptPages(transport, 2);
    RunContext context;
    // Trips once the probe and the first page request have been written.
    context.cancellation.SetPredicate([&transport] { return transport.WritesConsumed() >= 2; });

    auto built_plan_13 = ReadPlan();
    ASSERT_THAT(built_plan_13, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_13), transport, context), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationAfterTheFirstReadAbortsBeforeInspectingTheReply)
{
    ScriptedKlineFlashTransport transport;
    transport.Exchange(Request({0xbf}), InitReply());
    RunContext context;
    // Trips once the probe reply has actually been read, not before.
    context.cancellation.SetPredicate([&transport] { return !transport.read_timeouts.empty(); });

    auto built_plan_15 = ReadPlan();
    ASSERT_THAT(built_plan_15, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_15), transport, context), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationBeforeAPageRequestPropagatesThroughExchangeExpect)
{
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    RunContext context;
    // Plan admission is check 1; the probe checks cancellation before its
    // send, in the transport read, and after the read (calls 2-4). Trip on the
    // fifth check, which is the first page request's send.
    context.cancellation.CancelOnCheck(5);

    auto built_plan_16 = ReadPlan();
    ASSERT_THAT(built_plan_16, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_16), transport, context),
                IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before write")));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

class SubaruUnisiaJecsM32rKlineReadSizes
    : public ::testing::TestWithParam<std::tuple<std::string_view, std::string_view, std::uint32_t>>
{
};

TEST_P(SubaruUnisiaJecsM32rKlineReadSizes, ReadsEveryPageOfTheVariant)
{
    const auto& [protocol, mcu, rom_size] = GetParam();
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    ScriptPages(transport, rom_size / 0x80);
    RunContext context;

    auto built_plan_17 = ReadPlan(protocol, mcu);
    ASSERT_THAT(built_plan_17, fastecu::testing::IsOk());
    auto result = RunScripted(std::move(*built_plan_17), transport, context);

    ASSERT_THAT(result, IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(result->read_bytes->size(), rom_size);
    EXPECT_TRUE(transport.ScriptConsumed());
}

INSTANTIATE_TEST_SUITE_P(AllVariants, SubaruUnisiaJecsM32rKlineReadSizes,
                         ::testing::Values(std::tuple{"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000U},
                                           std::tuple{"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000U},
                                           std::tuple{"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000U},
                                           std::tuple{"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000U}),
                         (
                             [](const auto& param_info)
                             {
                                 static constexpr std::array kNames{
                                     "SubEcuUnisiaJecs20M32R128KB0x20000U", "SubEcuUnisiaJecs30M32R256KB0x40000U",
                                     "SubEcuUnisiaJecs40M32R384KB0x60000U", "SubEcuUnisiaJecs70M32R512KB0x80000U"};
                                 return kNames[param_info.index];
                             }));

TEST(SubaruUnisiaJecsM32rKlineExecutor, ReadsABootmodeProtocolWithTheSameWireSequence)
{
    ScriptedKlineFlashTransport transport;
    ScriptWarmEntry(transport);
    ScriptPages(transport, 0x40000 / 0x80);
    RunContext context;
    auto built_plan_18 = ReadPlan("sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB");
    ASSERT_THAT(built_plan_18, fastecu::testing::IsOk());
    const auto result = RunScripted(std::move(*built_plan_18), transport, context);
    ASSERT_THAT(result, IsOk());
    ASSERT_THAT(result->read_bytes, Optional(_));
    EXPECT_EQ(result->read_bytes->size(), 0x40000U);
    EXPECT_EQ(result->rom_id, std::optional<std::string>("123456789A_"));
    EXPECT_TRUE(transport.ScriptConsumed());
}

const bytes::Bytes& RomImage()
{
    static const bytes::Bytes kImage = []
    {
        bytes::Bytes out(kRomSize);
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            out[i] = static_cast<bytes::Byte>(i * 3 + 1);
        }
        return out;
    }();
    return kImage;
}

Result<FlashPlan> WritePlan()
{
    return BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kWrite, kProtocol, kMcu, RomImage(), false);
}

bytes::Bytes BlockRequest(std::uint32_t index)
{
    const std::uint32_t address = index * 0x80;
    const bool last = index == kRomSize / 0x80 - 1;
    bytes::Bytes payload{0xaf, static_cast<bytes::Byte>(last ? 0x69 : 0x61), static_cast<bytes::Byte>(address >> 16U),
                         static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address)};
    std::ranges::transform(std::views::iota(std::uint32_t{0}, std::uint32_t{0x80}), std::back_inserter(payload),
                           [address](std::uint32_t j)
                           { return static_cast<bytes::Byte>(RomImage()[address + j] ^ 0x82U); });
    return Request(payload);
}

const bytes::Bytes kEraseStarted = Reply({0xef, 0x42});
const bytes::Bytes kDone = Reply({0xef, 0x52});

void ScriptObkRunning(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.Section("OBK probe answered");
    transport.Exchange(Request({0xaf}), Reply({0xef}));
}

void ScriptColdFlashMode(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.Section("enter flash mode");
    transport.ExpectWrite(Request({0xaf}));
    transport.QueueNoFrame();
    transport.Exchange(Request({0xbf}), InitReply());
    transport.Exchange(Request({0xaf, 0x11, 0x12, 0x34, 0x56, 0x78, 0x9a, 0x02, 0x00, 0x00}), Reply({0xef}));
}

// One empty poll before each erase reply, then the trailing read.
void ScriptErase(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.Section("erase");
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueNoFrame();
    transport.QueueRead(kEraseStarted);
    transport.QueueNoFrame();
    transport.QueueRead(kDone);
    transport.QueueNoFrame();
}

void ScriptBlocks(ScriptedKlineFlashTransport& transport, std::uint32_t count)
{
    auto section = transport.Section("program blocks");
    for (std::uint32_t index = 0; index < count; ++index)
    {
        transport.Exchange(BlockRequest(index), kDone);
    }
}

std::uint32_t BlockCount()
{
    return kRomSize / 0x80;
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, WritesWhenTheKernelIsAlreadyRunning)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    ScriptBlocks(transport, BlockCount());
    RunContext context;

    auto built_plan_19 = WritePlan();
    ASSERT_THAT(built_plan_19, fastecu::testing::IsOk());
    auto result = RunScripted(std::move(*built_plan_19), transport, context);

    ASSERT_THAT(result, IsOk());
    EXPECT_EQ(result->operation, FlashOperation::kWrite);
    EXPECT_THAT(result->read_bytes, Eq(std::nullopt));
    EXPECT_THAT(result->rom_id, Eq(std::nullopt)) << "legacy set RomId only on read";
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, std::vector<int>{19200});
    EXPECT_EQ(transport.control_line_trace,
              (std::vector<Line>{Line::kEnableProgrammingVoltageLine, Line::kDisableLecLines}));
    // After the OBK probe, before AF 31 (write_mem()).
    EXPECT_EQ(transport.programming_voltage_line_write_index, std::optional<std::size_t>(1));
    // One 500 ms sleep after each empty erase poll.
    EXPECT_EQ(context.clock.Elapsed(), 1000ms);
    EXPECT_EQ(context.events.progress_calls.back(), (std::pair<int, int>{1024, 1024}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EntersFlashModeFromCold)
{
    ScriptedKlineFlashTransport transport;
    ScriptColdFlashMode(transport);
    ScriptErase(transport);
    ScriptBlocks(transport, BlockCount());
    RunContext context;

    auto built_plan_20 = WritePlan();
    ASSERT_THAT(built_plan_20, fastecu::testing::IsOk());
    ASSERT_THAT(RunScripted(std::move(*built_plan_20), transport, context), IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{19200, 4800, 19200}));
    EXPECT_EQ(transport.programming_voltage_line_write_index, std::optional<std::size_t>(3));
}

// A probe answer that is not EF means no kernel is running: legacy went on to
// the cold flash-mode entry (write_mem()).
TEST(SubaruUnisiaJecsM32rKlineExecutor, NonEfObkProbeReplyFallsBackToColdFlashMode)
{
    ScriptedKlineFlashTransport transport;
    {
        auto section = transport.Section("OBK probe rejected, then cold entry");
        transport.Exchange(Request({0xaf}), Reply({0x7f, 0xaf, 0x22}));
        transport.Exchange(Request({0xbf}), InitReply());
        transport.Exchange(Request({0xaf, 0x11, 0x12, 0x34, 0x56, 0x78, 0x9a, 0x02, 0x00, 0x00}), Reply({0xef}));
    }
    ScriptErase(transport);
    ScriptBlocks(transport, BlockCount());
    RunContext context;

    auto built_plan_21 = WritePlan();
    ASSERT_THAT(built_plan_21, fastecu::testing::IsOk());
    ASSERT_THAT(RunScripted(std::move(*built_plan_21), transport, context), IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.baud_calls, (std::vector<int>{19200, 4800, 19200}));
    EXPECT_EQ(transport.programming_voltage_line_write_index, std::optional<std::size_t>(3));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, RejectedFlashModeEntryNeverRaisesProgrammingVoltage)
{
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xaf}));
    transport.QueueNoFrame();
    transport.Exchange(Request({0xbf}), InitReply());
    transport.Exchange(Request({0xaf, 0x11, 0x12, 0x34, 0x56, 0x78, 0x9a, 0x02, 0x00, 0x00}),
                       Reply({0x7f, 0xaf, 0x22}));
    RunContext context;

    auto built_plan_22 = WritePlan();
    ASSERT_THAT(built_plan_22, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_22), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace, std::vector<Line>{Line::kDisableLecLines});
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, WriteFailsWhenTheInitialBaudChangeFails)
{
    ScriptedKlineFlashTransport transport;
    transport.set_baud_result = Fail(ErrorKind::kDisconnected, "adapter gone");
    RunContext context;

    auto built_plan_23 = WritePlan();
    ASSERT_THAT(built_plan_23, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_23), transport, context), IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(transport.ScriptConsumed());
    // execute() still drops the LEC line on this early exit.
    EXPECT_EQ(transport.control_line_trace, std::vector<Line>{Line::kDisableLecLines});
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationBeforeRaisingProgrammingVoltageAbortsTheWrite)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    RunContext context;
    // Plan admission is check 1; the OBK probe checks cancellation before its
    // send, in the transport read, and after the read (calls 2-4). Trip on the
    // fifth check, which is write_rom()'s own "before programming voltage" gate
    // (write_mem()).
    context.cancellation.CancelOnCheck(5);

    auto built_plan_24 = WritePlan();
    ASSERT_THAT(built_plan_24, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_24), transport, context),
                IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before programming voltage")));
    EXPECT_EQ(transport.control_line_trace, std::vector<Line>{Line::kDisableLecLines});
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, WriteFailsWhenProgrammingVoltageCannotBeRaised)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    transport.enable_programming_voltage_line_result = Fail(ErrorKind::kDisconnected, "VPP relay stuck");
    RunContext context;

    auto built_plan_25 = WritePlan();
    ASSERT_THAT(built_plan_25, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_25), transport, context), IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(transport.control_line_trace,
              (std::vector<Line>{Line::kEnableProgrammingVoltageLine, Line::kDisableLecLines}));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EraseStartExhaustionFails)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    transport.ExpectWrite(Request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.QueueNoFrame();
    }
    RunContext context;

    auto built_plan_26 = WritePlan();
    ASSERT_THAT(built_plan_26, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_26), transport, context), IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(context.clock.Elapsed(), 20 * 500ms);
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EraseCompleteExhaustionFailsInsteadOfProgramming)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(kEraseStarted);
    for (int round = 0; round < 40; ++round)
    {
        transport.QueueNoFrame();
    }
    RunContext context;

    auto built_plan_27 = WritePlan();
    ASSERT_THAT(built_plan_27, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_27), transport, context), IsErr(ErrorKind::kTimeout));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.WritesConsumed(), 2U) << "legacy went on to program blank flash";
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, UnexpectedEraseReplyFails)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(Reply({0xef, 0x48}));
    RunContext context;

    auto built_plan_28 = WritePlan();
    ASSERT_THAT(built_plan_28, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_28), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, BadBlockReplyStopsBeforeTheNextBlock)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    transport.Exchange(BlockRequest(0), Reply({0xef, 0x5a}));
    RunContext context;

    auto built_plan_29 = WritePlan();
    ASSERT_THAT(built_plan_29, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_29), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

// is_exact_reply's length half: a well-formed EF 52 that carries a trailing
// byte is not the exact acknowledgement.
TEST(SubaruUnisiaJecsM32rKlineExecutor, LongerBlockReplyStopsBeforeTheNextBlock)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    transport.Exchange(BlockRequest(0), Reply({0xef, 0x52, 0x00}));
    RunContext context;

    auto built_plan_30 = WritePlan();
    ASSERT_THAT(built_plan_30, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_30), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(transport.WritesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, SilentNonFinalBlockTimesOutBeforeTheNextBlock)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    transport.Exchange(BlockRequest(0), kDone);
    transport.ExpectWrite(BlockRequest(1));
    transport.QueueNoFrame();
    RunContext context;

    auto built_plan_31 = WritePlan();
    ASSERT_THAT(built_plan_31, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_31), transport, context), IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 4U);
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, FinalBlockSilenceSucceedsWithAWarning)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    ScriptBlocks(transport, BlockCount() - 1);
    transport.ExpectWrite(BlockRequest(BlockCount() - 1));
    transport.QueueNoFrame();
    RunContext context;

    auto built_plan_32 = WritePlan();
    ASSERT_THAT(built_plan_32, fastecu::testing::IsOk());
    ASSERT_THAT(RunScripted(std::move(*built_plan_32), transport, context), IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(context.events.logs, Contains(Pair(LogLevel::kWarning, _)));
    EXPECT_EQ(transport.read_timeouts.back(), 3000ms);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, FinalBlockBadReplyFails)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    ScriptBlocks(transport, BlockCount() - 1);
    transport.Exchange(BlockRequest(BlockCount() - 1), Reply({0x7f, 0xaf, 0x22}));
    RunContext context;

    auto built_plan_33 = WritePlan();
    ASSERT_THAT(built_plan_33, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_33), transport, context), IsErr(ErrorKind::kBadResponse));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationDuringProgrammingReportsCancelledAndDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    ScriptBlocks(transport, 2);
    RunContext context;
    // Trips as soon as the first block has been written.
    context.cancellation.SetPredicate([&transport] { return transport.WritesConsumed() >= 3; });

    auto built_plan_34 = WritePlan();
    ASSERT_THAT(built_plan_34, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_34), transport, context), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 3U);
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancellationDuringTheErasePollDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    transport.ExpectWrite(Request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.QueueNoFrame();
    }
    RunContext context;
    // Trips on the third erase poll.
    context.cancellation.SetPredicate([&transport] { return transport.read_timeouts.size() >= 4; });

    auto built_plan_35 = WritePlan();
    ASSERT_THAT(built_plan_35, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_35), transport, context), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
    EXPECT_EQ(transport.control_line_trace,
              (std::vector<Line>{Line::kEnableProgrammingVoltageLine, Line::kDisableLecLines}));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, TransportErrorMidProgrammingPropagatesAndDropsTheLine)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    transport.ExpectWrite(BlockRequest(0));
    transport.QueueError(ErrorKind::kDisconnected, "adapter unplugged");
    RunContext context;

    auto built_plan_36 = WritePlan();
    ASSERT_THAT(built_plan_36, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_36), transport, context), IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CleanupFailureFailsAnOtherwiseSuccessfulWrite)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    ScriptErase(transport);
    ScriptBlocks(transport, BlockCount());
    transport.disable_lec_lines_result = Fail(ErrorKind::kDisconnected, "RTS stuck");
    RunContext context;

    auto built_plan_37 = WritePlan();
    ASSERT_THAT(built_plan_37, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_37), transport, context), IsErr(ErrorKind::kDisconnected));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CleanupFailureNeverReplacesAnEarlierError)
{
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(Reply({0xef, 0x48}));
    transport.disable_lec_lines_result = Fail(ErrorKind::kDisconnected, "RTS stuck");
    RunContext context;

    auto built_plan_38 = WritePlan();
    ASSERT_THAT(built_plan_38, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_38), transport, context), IsErr(ErrorKind::kBadResponse));
}
class FirstWriteFaultTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit FirstWriteFaultTransport(bool short_write) : short_write_(short_write)
    {
    }

    Result<std::size_t> Write(bytes::ByteView data) override
    {
        auto written = ScriptedKlineFlashTransport::Write(data);
        if (!written.has_value())
        {
            return written;
        }
        if (short_write_)
        {
            return data.size() - 1;
        }
        return Fail(ErrorKind::kDisconnected, "write fault");
    }

    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        ++read_calls;
        return ScriptedKlineFlashTransport::Read(timeout, cancellation);
    }

    int read_calls = 0;

  private:
    bool short_write_;
};

TEST(SubaruUnisiaJecsM32rKlineExecutor, ShortWriteStopsBeforeReceiving)
{
    FirstWriteFaultTransport transport(true);
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xaf, 0x30});
    RunContext context;

    auto built_plan_39 = WritePlan();
    ASSERT_THAT(built_plan_39, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_39), transport, context),
                IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("short K-Line write")));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_EQ(transport.read_calls, 0);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(transport.control_line_trace, ::testing::ElementsAre(Line::kDisableLecLines));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, WriteFailureStopsBeforeReceiving)
{
    FirstWriteFaultTransport transport(false);
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x01, 0xaf, 0x30});
    RunContext context;

    auto built_plan_40 = WritePlan();
    ASSERT_THAT(built_plan_40, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_40), transport, context),
                IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("write fault")));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_EQ(transport.read_calls, 0);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(transport.control_line_trace, ::testing::ElementsAre(Line::kDisableLecLines));
}

// Fails the k-th write, read and baud change in turn, for the first `prefix`
// calls of each (the page and block loops repeat one exchange shape), and
// expects the fault to propagate with nothing written afterwards.
void ExpectEveryEarlyFaultPropagates(const std::function<Result<FlashPlan>()>& make_plan,
                                     const std::function<void(ScriptedKlineFlashTransport&)>& script,
                                     std::size_t prefix)
{
    for (const KlineFaultOp op : {KlineFaultOp::kWrite, KlineFaultOp::kRead, KlineFaultOp::kSetBaud})
    {
        SCOPED_TRACE(::testing::Message() << "op " << static_cast<int>(op));
        std::size_t count = 0;
        {
            FaultingKlineFlashTransport transport;
            transport.op = op;
            script(transport);
            RunContext context;
            auto plan = make_plan();
            ASSERT_THAT(plan, IsOk());
            std::ignore = RunScripted(*plan, transport, context); // only the call count matters here
            count = std::min(transport.calls, prefix);
        }
        ASSERT_GT(count, 0U);
        for (std::size_t k = 1; k <= count; ++k)
        {
            SCOPED_TRACE(::testing::Message() << "call " << k);
            FaultingKlineFlashTransport transport;
            transport.op = op;
            transport.fail_at = k;
            script(transport);
            RunContext context;
            auto plan = make_plan();
            ASSERT_THAT(plan, IsOk());
            EXPECT_THAT(RunScripted(*plan, transport, context),
                        IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
            EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>{transport.WritesConsumed()});
        }
    }
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EveryEarlyTransportFaultInAColdReadPropagates)
{
    ExpectEveryEarlyFaultPropagates([] { return ReadPlan(); },
                                    [](ScriptedKlineFlashTransport& transport)
                                    {
                                        ScriptColdEntry(transport);
                                        ScriptPages(transport, kRomSize / 0x80);
                                    },
                                    6);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, EveryEarlyTransportFaultInAColdWritePropagates)
{
    ExpectEveryEarlyFaultPropagates([] { return WritePlan(); },
                                    [](ScriptedKlineFlashTransport& transport)
                                    {
                                        ScriptColdFlashMode(transport);
                                        ScriptErase(transport);
                                        ScriptBlocks(transport, BlockCount());
                                    },
                                    10);
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, FrameAfterEraseIsLoggedAndDiscarded)
{
    // write_mem() read once more after the erase and dropped the result.
    ScriptedKlineFlashTransport transport;
    ScriptObkRunning(transport);
    {
        auto section = transport.Section("erase with a trailing frame");
        transport.ExpectWrite(Request({0xaf, 0x31}));
        transport.QueueRead(kEraseStarted);
        transport.QueueRead(kDone);
        transport.QueueRead(kDone);
    }
    ScriptBlocks(transport, BlockCount());
    RunContext context;

    auto plan = WritePlan();
    ASSERT_THAT(plan, IsOk());
    ASSERT_THAT(RunScripted(*plan, transport, context), IsOk());
    EXPECT_THAT(context.events.logs, ::testing::Contains(::testing::Pair(
                                         LogLevel::kDebug, ::testing::HasSubstr("Discarded after erase: "))));
}

TEST(SubaruUnisiaJecsM32rKlineExecutor, CancelledAtEntryTouchesNoControlLine)
{
    ScriptedKlineFlashTransport transport;
    RunContext context;
    context.cancellation.SetCancelled(true);

    auto plan = WritePlan();
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(RunScripted(std::move(*plan), transport, context),
                IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_THAT(transport.control_line_trace, IsEmpty());
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

} // namespace
} // namespace fastecu::flash
