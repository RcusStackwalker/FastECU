#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/testing/faulting_kline_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/faulting_clock.h"
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
using ::testing::IsEmpty;
using Line = ScriptedKlineFlashTransport::ControlLineAction;
using Op = ScriptedKlineFlashTransport::Operation;

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

bytes::Bytes Rom(std::uint32_t size)
{
    bytes::Bytes image(size);
    for (std::uint32_t i = 0; i < size; ++i)
    {
        image[i] = static_cast<bytes::Byte>(i * 13 + 7);
    }
    return image;
}

Result<FlashPlan> ProgramPlan(std::string_view protocol = "sub_ecu_unisia_jecs_20_bootmode",
                              std::string_view mcu = "M32R_128KB", std::uint32_t size = 0x20000)
{
    return BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(FlashOperation::kWrite, protocol, mcu, Rom(size));
}

// AF 61|69 <addr24> <128 bytes as-is>.
bytes::Bytes BlockRequest(const FlashPlan& plan, std::uint32_t block, bool last)
{
    const std::uint32_t address = block * 0x80;
    bytes::Bytes payload{0xaf, static_cast<bytes::Byte>(last ? 0x69 : 0x61), static_cast<bytes::Byte>(address >> 16U),
                         static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address)};
    payload.insert(payload.end(), plan.ImageOrEmpty().begin() + address, plan.ImageOrEmpty().begin() + address + 0x80);
    return Request(payload);
}

// One empty poll in each loop, so both poll sleeps are exercised.
void ScriptErase(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.Section("erase");
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueNoFrame();
    transport.QueueRead(Reply({0xef, 0x42}));
    transport.QueueNoFrame();
    transport.QueueRead(Reply({0xef, 0x52}));
}

void ScriptBlocks(ScriptedKlineFlashTransport& transport, const FlashPlan& plan, std::uint32_t upto)
{
    auto section = transport.Section("blocks");
    for (std::uint32_t block = 0; block < upto; ++block)
    {
        transport.Exchange(BlockRequest(plan, block, false), Reply({0xef, 0x52}));
    }
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
    return SubaruUnisiaJecsM32rBootModeProgramExecutor{}.Execute(plan, transport, context.clock, context.cancellation,
                                                                 context.events);
}

bool Logged(const RunContext& context, LogLevel level)
{
    return std::ranges::any_of(context.events.logs, [level](const auto& log) { return log.first == level; });
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, TransportSetupIs19200BaudNoParity)
{
    auto built_plan_1 = ProgramPlan();
    ASSERT_THAT(built_plan_1, fastecu::testing::IsOk());
    const auto setup = SubaruUnisiaJecsM32rBootModeProgramExecutor{}.TransportSetup(std::move(*built_plan_1));
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 19200);
    EXPECT_EQ(setup->parity, KlineParity::kNone);
    EXPECT_FALSE(setup->iso14230);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, BeforeConfigureResetsThenClearsTheHeader)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruUnisiaJecsM32rBootModeProgramExecutor{}.BeforeTransportConfigure(transport, clock, cancellation),
                IsOk());
    EXPECT_EQ(transport.lifecycle_calls, std::vector<std::string>{"reset_connection"});
    EXPECT_EQ(transport.header_mode_calls, std::vector<bool>{false});
}

class ProgramsTheWholeRom
    : public ::testing::TestWithParam<std::tuple<std::string_view, std::string_view, std::uint32_t>>
{
};

TEST_P(ProgramsTheWholeRom, ErasesThenWritesEveryBlock)
{
    const auto [protocol, mcu, size] = GetParam();
    auto built_plan_2 = ProgramPlan(protocol, mcu, size);
    ASSERT_THAT(built_plan_2, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_2);
    const std::uint32_t blocks = size / 0x80;
    ScriptedKlineFlashTransport transport;
    ScriptErase(transport);
    ScriptBlocks(transport, plan, blocks - 1);
    transport.Exchange(BlockRequest(plan, blocks - 1, true), Reply({0xef, 0x52}));
    RunContext context;

    ASSERT_THAT(RunScripted(plan, transport, context), IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.control_line_trace, (std::vector{Line::kEnableProgrammingVoltageLine, Line::kDisableLecLines}));
    EXPECT_EQ(transport.programming_voltage_line_write_index, std::optional<std::size_t>(0));
    // AF 31 settle 500, one empty start poll 500, one empty done poll 1000,
    // post-erase 1000, then 10 ms after every AF 61.
    EXPECT_EQ(context.clock.Elapsed(), 500ms + 500ms + 1000ms + 1000ms + 10ms * (blocks - 1));
    EXPECT_EQ(std::ranges::count(transport.operation_trace, Op::kRead10), 4);
    EXPECT_EQ(context.events.progress_calls.back(),
              (std::pair<int, int>{static_cast<int>(blocks), static_cast<int>(blocks)}));
    EXPECT_FALSE(Logged(context, LogLevel::kWarning));
}

INSTANTIATE_TEST_SUITE_P(BothBootmodeRoms, ProgramsTheWholeRom,
                         ::testing::Values(std::make_tuple("sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000U),
                                           std::make_tuple("sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000U)),
                         (
                             [](const auto& param_info)
                             {
                                 static constexpr std::array kNames{"SubEcuUnisiaJecs20BootmodeM32R128KB0x20000U",
                                                                    "SubEcuUnisiaJecs30BootmodeM32R256KB0x40000U"};
                                 return kNames[param_info.index];
                             }));

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, SilenceAfterTheFinalBlockSucceedsWithAWarning)
{
    auto built_plan_3 = ProgramPlan();
    ASSERT_THAT(built_plan_3, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_3);
    ScriptedKlineFlashTransport transport;
    ScriptErase(transport);
    ScriptBlocks(transport, plan, 1023);
    transport.ExpectWrite(BlockRequest(plan, 1023, true));
    transport.QueueNoFrame();
    RunContext context;
    ASSERT_THAT(RunScripted(plan, transport, context), IsOk());
    EXPECT_TRUE(Logged(context, LogLevel::kWarning));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, ABadFinalBlockReplyFails)
{
    auto built_plan_4 = ProgramPlan();
    ASSERT_THAT(built_plan_4, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_4);
    ScriptedKlineFlashTransport transport;
    ScriptErase(transport);
    ScriptBlocks(transport, plan, 1023);
    transport.Exchange(BlockRequest(plan, 1023, true), Reply({0xef, 0x5c}));
    RunContext context;
    const auto result = RunScripted(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::HasSubstr("checksum error")));
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, EraseStartExhaustionFails)
{
    auto built_plan_5 = ProgramPlan();
    ASSERT_THAT(built_plan_5, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_5);
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.QueueNoFrame();
    }
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(context.clock.Elapsed(), 500ms + 20 * 500ms);
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, EraseDoneExhaustionFailsInsteadOfProgramming)
{
    auto built_plan_6 = ProgramPlan();
    ASSERT_THAT(built_plan_6, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_6);
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(Reply({0xef, 0x42}));
    for (int round = 0; round < 20; ++round)
    {
        transport.QueueNoFrame();
    }
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, AWrongEraseFrameFailsAndNamesTheStatus)
{
    auto built_plan_7 = ProgramPlan();
    ASSERT_THAT(built_plan_7, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_7);
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(Reply({0xef, 0x48}));
    RunContext context;
    const auto result = RunScripted(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result, fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::HasSubstr("missing VPP")));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, NamedAndUnknownStatusesInBlockReplies)
{
    for (const auto& [status, text] : std::to_array<std::pair<bytes::Byte, std::string_view>>({
             {0x72, "address error"},
             {0x8a, "FENTRY bit not set"},
             {0x5a, "unknown"},
         }))
    {
        SCOPED_TRACE(::testing::PrintToString(status));
        auto built_plan_8 = ProgramPlan();
        ASSERT_THAT(built_plan_8, fastecu::testing::IsOk());
        const FlashPlan plan = std::move(*built_plan_8);
        ScriptedKlineFlashTransport transport;
        ScriptErase(transport);
        transport.Exchange(BlockRequest(plan, 0, false), Reply({0xef, status}));
        RunContext context;
        const auto result = RunScripted(plan, transport, context);
        ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
        EXPECT_THAT(result,
                    fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::HasSubstr(std::string(text))));
    }
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, SilenceAfterANonFinalBlockTimesOut)
{
    auto built_plan_9 = ProgramPlan();
    ASSERT_THAT(built_plan_9, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_9);
    ScriptedKlineFlashTransport transport;
    ScriptErase(transport);
    transport.ExpectWrite(BlockRequest(plan, 0, false));
    transport.QueueNoFrame();
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancelledBeforeVoltageNeverRaisesIt)
{
    auto built_plan_10 = ProgramPlan();
    ASSERT_THAT(built_plan_10, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_10);
    ScriptedKlineFlashTransport transport;
    RunContext context;
    context.cancellation.SetCancelled(true);
    EXPECT_THAT(RunScripted(plan, transport, context),
                IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_THAT(transport.control_line_trace, IsEmpty());
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancelledMidProgrammingReportsCancelled)
{
    auto built_plan_11 = ProgramPlan();
    ASSERT_THAT(built_plan_11, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_11);
    ScriptedKlineFlashTransport transport;
    ScriptErase(transport);
    ScriptBlocks(transport, plan, 3);
    RunContext context;
    context.cancellation.SetPredicate([&transport] { return transport.WritesConsumed() >= 3; });
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CleanupFailureNeverReplacesAnEarlierError)
{
    auto built_plan_12 = ProgramPlan();
    ASSERT_THAT(built_plan_12, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_12);
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(Reply({0xef, 0x48}));
    transport.disable_lec_lines_result = Fail(ErrorKind::kDisconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kBadResponse));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, BlockRepliesWaitThreeSeconds)
{
    auto built_plan_13 = ProgramPlan();
    ASSERT_THAT(built_plan_13, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_13);
    ScriptedKlineFlashTransport transport;
    ScriptErase(transport);
    transport.ExpectWrite(BlockRequest(plan, 0, false));
    transport.QueueNoFrame();
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kTimeout));
    // The four erase polls (write_mem()) use the 10 ms kPollRead budget; every
    // read after the first block write uses the 3000 ms
    // serial_read_extra_long_timeout.
    ASSERT_EQ(transport.read_timeouts.size(), 5U);
    for (std::size_t i = 0; i < 4; ++i)
    {
        SCOPED_TRACE(::testing::PrintToString(i));
        EXPECT_EQ(transport.read_timeouts[i], 10ms);
    }
    EXPECT_EQ(transport.read_timeouts.back(), 3000ms);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancellationDuringEraseDropsTheLines)
{
    // ICancellationToken::cancelled() call count at which cancellation lands: 3
    // is the 500 ms post-AF-31 settle sleep (write_mem()), 4 is the
    // erase-started poll's first read, 9 is the erase-complete poll's first
    // read. (Full call sequence: 1 plan admission, 2 before the AF 31 write, 3
    // the settle sleep, 4/5 the erase-started poll's first read and its
    // post-read check, 6 that poll's retry sleep, 7/8 its second read and
    // post-read check, 9/10 the erase-complete poll's first read and post-read
    // check.)
    for (const std::size_t checkpoint : {3U, 4U, 9U})
    {
        SCOPED_TRACE(::testing::PrintToString(checkpoint));
        auto built_plan_14 = ProgramPlan();
        ASSERT_THAT(built_plan_14, fastecu::testing::IsOk());
        const FlashPlan plan = std::move(*built_plan_14);
        ScriptedKlineFlashTransport transport;
        ScriptErase(transport);
        RunContext context;
        context.cancellation.CancelOnCheck(checkpoint);
        const auto result = RunScripted(plan, transport, context);
        ASSERT_THAT(result, IsErr(ErrorKind::kCancelled)) << "checkpoint " << checkpoint;
        EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines) << "checkpoint " << checkpoint;
        // Only AF 31 was written; the block loop was never entered.
        EXPECT_EQ(transport.WritesConsumed(), 1U) << "checkpoint " << checkpoint;
    }
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, ANonStatusReplyFailsTheErasePoll)
{
    auto built_plan_15 = ProgramPlan();
    ASSERT_THAT(built_plan_15, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_15);
    ScriptedKlineFlashTransport transport;
    const bytes::Bytes malformed = Reply({0x7f, 0x31});
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(malformed);
    RunContext context;
    const auto result = RunScripted(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::HasSubstr(bytes::ToHex(malformed))));
    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::Not(::testing::HasSubstr("status"))));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, ABadChecksumFailsTheErasePoll)
{
    auto built_plan_16 = ProgramPlan();
    ASSERT_THAT(built_plan_16, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_16);
    ScriptedKlineFlashTransport transport;
    bytes::Bytes bad_checksum = Reply({0xef, 0x42});
    ++bad_checksum.back();
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(bad_checksum);
    RunContext context;
    const auto result = RunScripted(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::HasSubstr(bytes::ToHex(bad_checksum))));
    EXPECT_THAT(result,
                fastecu::testing::IsErrWith(ErrorKind::kBadResponse, ::testing::Not(::testing::HasSubstr("status"))));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, RejectsAKernelPlan)
{
    auto plan = BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(FlashOperation::kWrite, "sub_ecu_unisia_jecs_20_bootmode",
                                                            "M32R_128KB", bytes::Bytes(0x80, 0x00));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeProgramExecutor{}.TransportSetup(*plan), IsErr(ErrorKind::kInvalidConfig));
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

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, ShortWriteStopsBeforeReceiving)
{
    FirstWriteFaultTransport transport(true);
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x02, 0xaf, 0x31, 0x62});
    RunContext context;

    auto built_plan_17 = ProgramPlan();
    ASSERT_THAT(built_plan_17, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_17), transport, context),
                IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("short K-Line write")));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_EQ(transport.read_calls, 0);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(transport.control_line_trace,
                ::testing::ElementsAre(Line::kEnableProgrammingVoltageLine, Line::kDisableLecLines));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, WriteFailureStopsBeforeReceiving)
{
    FirstWriteFaultTransport transport(false);
    transport.ExpectWrite(bytes::Bytes{0x80, 0x10, 0xf0, 0x02, 0xaf, 0x31, 0x62});
    RunContext context;

    auto built_plan_18 = ProgramPlan();
    ASSERT_THAT(built_plan_18, fastecu::testing::IsOk());
    EXPECT_THAT(RunScripted(std::move(*built_plan_18), transport, context),
                IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("write fault")));
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_EQ(transport.read_calls, 0);
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(transport.control_line_trace,
                ::testing::ElementsAre(Line::kEnableProgrammingVoltageLine, Line::kDisableLecLines));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, AResetFailureSkipsTheHeaderChange)
{
    ScriptedKlineFlashTransport transport;
    transport.reset_result = Fail(ErrorKind::kDisconnected, "reset");
    FakeClock clock;
    FakeCancellationToken cancellation;
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeProgramExecutor{}.BeforeTransportConfigure(transport, clock, cancellation),
                IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("reset")));
    EXPECT_THAT(transport.header_mode_calls, IsEmpty());
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, AVoltageFailureNeverErases)
{
    auto built_plan = ProgramPlan();
    ASSERT_THAT(built_plan, IsOk());
    ScriptedKlineFlashTransport transport;
    transport.enable_programming_voltage_line_result = Fail(ErrorKind::kDisconnected, "vpp");
    RunContext context;
    EXPECT_THAT(RunScripted(*built_plan, transport, context),
                IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("vpp")));
    EXPECT_EQ(transport.WritesConsumed(), 0U);
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, EverySleepFailureStopsTheWrite)
{
    auto built_plan = ProgramPlan();
    ASSERT_THAT(built_plan, IsOk());
    // Sleeps: AF 31 settle, start poll, done poll, post-erase, block 0 pacing.
    for (const auto& [fail_at, writes] : std::to_array<std::pair<std::size_t, std::size_t>>({
             {1, 1},
             {2, 1},
             {3, 1},
             {4, 1},
             {5, 2},
         }))
    {
        SCOPED_TRACE(::testing::PrintToString(fail_at));
        ScriptedKlineFlashTransport transport;
        ScriptErase(transport);
        ScriptBlocks(transport, *built_plan, 1);
        FaultingClock clock;
        clock.fail_at = fail_at;
        FakeCancellationToken cancellation;
        RecordingEventSink events;
        EXPECT_THAT(
            SubaruUnisiaJecsM32rBootModeProgramExecutor{}.Execute(*built_plan, transport, clock, cancellation, events),
            IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("sleep failed")));
        EXPECT_EQ(transport.WritesConsumed(), writes);
        EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
    }
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, ABlockWriteFailureStopsBeforeReceiving)
{
    auto built_plan = ProgramPlan();
    ASSERT_THAT(built_plan, IsOk());
    // Write 1 is AF 31; writes 2 and 3 are blocks 0 and 1, after four erase polls.
    for (const std::size_t fail_at : {2U, 3U})
    {
        SCOPED_TRACE(::testing::PrintToString(fail_at));
        FaultingKlineFlashTransport transport;
        transport.op = KlineFaultOp::kWrite;
        transport.fail_at = fail_at;
        ScriptErase(transport);
        ScriptBlocks(transport, *built_plan, 2);
        RunContext context;
        EXPECT_THAT(RunScripted(*built_plan, transport, context),
                    IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
        EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>(transport.WritesConsumed()));
        EXPECT_EQ(transport.read_timeouts.size(), fail_at + 2);
        EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
    }
}

} // namespace
} // namespace fastecu::flash
