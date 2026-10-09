#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
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
using fastecu::testing::IsOk;
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

FlashPlan ProgramPlan(std::string_view protocol = "sub_ecu_unisia_jecs_20_bootmode",
                      std::string_view mcu = "M32R_128KB", std::uint32_t size = 0x20000)
{
    auto plan = BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(FlashOperation::kWrite, protocol, mcu, Rom(size));
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

// write_mem() :485-512: AF 61|69 <addr24> <128 bytes as-is>.
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
    const auto setup = SubaruUnisiaJecsM32rBootModeProgramExecutor{}.TransportSetup(ProgramPlan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 19200);                // write_mem() :364
    EXPECT_EQ(setup->parity, KlineParity::kNone); // write_mem() :362
    EXPECT_FALSE(setup->iso14230);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, BeforeConfigureResetsThenClearsTheHeader)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruUnisiaJecsM32rBootModeProgramExecutor{}.BeforeTransportConfigure(transport, clock, cancellation),
                IsOk());
    EXPECT_EQ(transport.lifecycle_calls, std::vector<std::string>{"reset_connection"}); // write_mem() :361
    EXPECT_EQ(transport.header_mode_calls, std::vector<bool>{false});
}

class ProgramsTheWholeRom
    : public ::testing::TestWithParam<std::tuple<std::string_view, std::string_view, std::uint32_t>>
{
};

TEST_P(ProgramsTheWholeRom, ErasesThenWritesEveryBlock)
{
    const auto [protocol, mcu, size] = GetParam();
    const FlashPlan plan = ProgramPlan(protocol, mcu, size);
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
    // post-erase 1000, then 10 ms after every AF 61 (:370-465, :539).
    EXPECT_EQ(context.clock.Elapsed(), 500ms + 500ms + 1000ms + 1000ms + 10ms * (blocks - 1));
    EXPECT_EQ(std::ranges::count(transport.operation_trace, Op::kRead10), 4);
    EXPECT_EQ(context.events.progress_calls.back(),
              (std::pair<int, int>{static_cast<int>(blocks), static_cast<int>(blocks)}));
    EXPECT_FALSE(Logged(context, LogLevel::kWarning));
}

INSTANTIATE_TEST_SUITE_P(BothBootmodeRoms, ProgramsTheWholeRom,
                         ::testing::Values(std::make_tuple("sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000U),
                                           std::make_tuple("sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000U)));

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, SilenceAfterTheFinalBlockSucceedsWithAWarning)
{
    const FlashPlan plan = ProgramPlan();
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
    const FlashPlan plan = ProgramPlan();
    ScriptedKlineFlashTransport transport;
    ScriptErase(transport);
    ScriptBlocks(transport, plan, 1023);
    transport.Exchange(BlockRequest(plan, 1023, true), Reply({0xef, 0x5c}));
    RunContext context;
    const auto result = RunScripted(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("checksum error"));
    EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, EraseStartExhaustionFails)
{
    const FlashPlan plan = ProgramPlan();
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
    const FlashPlan plan = ProgramPlan();
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
    const FlashPlan plan = ProgramPlan();
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(Reply({0xef, 0x48}));
    RunContext context;
    const auto result = RunScripted(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("missing VPP"));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, NamedAndUnknownStatusesInBlockReplies)
{
    for (const auto& [status, text] : std::to_array<std::pair<bytes::Byte, std::string_view>>({
             {0x72, "address error"},
             {0x8a, "FENTRY bit not set"},
             {0x5a, "unknown"},
         }))
    {
        const FlashPlan plan = ProgramPlan();
        ScriptedKlineFlashTransport transport;
        ScriptErase(transport);
        transport.Exchange(BlockRequest(plan, 0, false), Reply({0xef, status}));
        RunContext context;
        const auto result = RunScripted(plan, transport, context);
        ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
        EXPECT_THAT(result.error().detail, ::testing::HasSubstr(std::string(text)));
    }
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, SilenceAfterANonFinalBlockTimesOut)
{
    const FlashPlan plan = ProgramPlan();
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
    const FlashPlan plan = ProgramPlan();
    ScriptedKlineFlashTransport transport;
    RunContext context;
    context.cancellation.SetCancelled(true);
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.control_line_trace, std::vector{Line::kDisableLecLines});
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancelledMidProgrammingReportsCancelled)
{
    const FlashPlan plan = ProgramPlan();
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
    const FlashPlan plan = ProgramPlan();
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(Reply({0xef, 0x48}));
    transport.disable_lec_lines_result = Fail(ErrorKind::kDisconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kBadResponse));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, BlockRepliesWaitThreeSeconds)
{
    const FlashPlan plan = ProgramPlan();
    ScriptedKlineFlashTransport transport;
    ScriptErase(transport);
    transport.ExpectWrite(BlockRequest(plan, 0, false));
    transport.QueueNoFrame();
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kTimeout));
    // The four erase polls (write_mem() :400, :442) use the 10 ms kPollRead
    // budget; every read after the first block write uses the 3000 ms
    // serial_read_extra_long_timeout (:518).
    ASSERT_EQ(transport.read_timeouts.size(), 5U);
    for (std::size_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(transport.read_timeouts[i], 10ms);
    }
    EXPECT_EQ(transport.read_timeouts.back(), 3000ms);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancellationDuringEraseDropsTheLines)
{
    // ICancellationToken::cancelled() call count at which cancellation lands:
    // 3 is the 500 ms post-AF-31 settle sleep (write_mem() :389), 4 is the
    // erase-started poll's first read, 9 is the erase-complete poll's first
    // read. (Full call sequence: 1 before-voltage check, 2 before the AF 31
    // write, 3 the settle sleep, 4/5 the erase-started poll's first read and
    // its post-read check, 6 that poll's retry sleep, 7/8 its second read and
    // post-read check, 9/10 the erase-complete poll's first read and
    // post-read check.)
    for (const std::size_t checkpoint : {3U, 4U, 9U})
    {
        const FlashPlan plan = ProgramPlan();
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
    const FlashPlan plan = ProgramPlan();
    ScriptedKlineFlashTransport transport;
    const bytes::Bytes malformed = Reply({0x7f, 0x31});
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(malformed);
    RunContext context;
    const auto result = RunScripted(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr(bytes::ToHex(malformed)));
    EXPECT_THAT(result.error().detail, ::testing::Not(::testing::HasSubstr("status")));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, ABadChecksumFailsTheErasePoll)
{
    const FlashPlan plan = ProgramPlan();
    ScriptedKlineFlashTransport transport;
    bytes::Bytes bad_checksum = Reply({0xef, 0x42});
    ++bad_checksum.back();
    transport.ExpectWrite(Request({0xaf, 0x31}));
    transport.QueueRead(bad_checksum);
    RunContext context;
    const auto result = RunScripted(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr(bytes::ToHex(bad_checksum)));
    EXPECT_THAT(result.error().detail, ::testing::Not(::testing::HasSubstr("status")));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, RejectsAKernelPlan)
{
    auto plan = BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(FlashOperation::kWrite, "sub_ecu_unisia_jecs_20_bootmode",
                                                            "M32R_128KB", bytes::Bytes(0x80, 0x00));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeProgramExecutor{}.TransportSetup(*plan), IsErr(ErrorKind::kInvalidConfig));
}
} // namespace
} // namespace fastecu::flash
