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
bytes::Bytes request(const bytes::Bytes& payload)
{
    bytes::Bytes frame{0x80, 0x10, 0xf0, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(bytes::sum8(frame));
    return frame;
}

bytes::Bytes reply(const bytes::Bytes& payload)
{
    bytes::Bytes frame{0x80, 0xf0, 0x10, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(bytes::sum8(frame));
    return frame;
}

bytes::Bytes rom(std::uint32_t size)
{
    bytes::Bytes image(size);
    for (std::uint32_t i = 0; i < size; ++i)
    {
        image[i] = static_cast<bytes::Byte>(i * 13 + 7);
    }
    return image;
}

FlashPlan program_plan(std::string_view protocol = "sub_ecu_unisia_jecs_20_bootmode",
                       std::string_view mcu = "M32R_128KB", std::uint32_t size = 0x20000)
{
    auto plan = build_subaru_unisia_jecs_m32r_bootmode_program_plan(FlashOperation::Write, protocol, mcu, rom(size));
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

// write_mem() :485-512: AF 61|69 <addr24> <128 bytes as-is>.
bytes::Bytes block_request(const FlashPlan& plan, std::uint32_t block, bool last)
{
    const std::uint32_t address = block * 0x80;
    bytes::Bytes payload{0xaf, static_cast<bytes::Byte>(last ? 0x69 : 0x61), static_cast<bytes::Byte>(address >> 16U),
                         static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address)};
    payload.insert(payload.end(), plan.image()->begin() + address, plan.image()->begin() + address + 0x80);
    return request(payload);
}

// One empty poll in each loop, so both poll sleeps are exercised.
void script_erase(ScriptedKlineFlashTransport& transport)
{
    auto section = transport.section("erase");
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queue_no_frame();
    transport.queueRead(reply({0xef, 0x42}));
    transport.queue_no_frame();
    transport.queueRead(reply({0xef, 0x52}));
}

void script_blocks(ScriptedKlineFlashTransport& transport, const FlashPlan& plan, std::uint32_t upto)
{
    auto section = transport.section("blocks");
    for (std::uint32_t block = 0; block < upto; ++block)
    {
        transport.exchange(block_request(plan, block, false), reply({0xef, 0x52}));
    }
}

struct RunContext
{
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
};

Result<FlashExecutionResult> run(const FlashPlan& plan, ScriptedKlineFlashTransport& transport, RunContext& context)
{
    return SubaruUnisiaJecsM32rBootModeProgramExecutor{}.execute(plan, transport, context.clock, context.cancellation,
                                                                 context.events);
}

bool logged(const RunContext& context, LogLevel level)
{
    return std::ranges::any_of(context.events.logs, [level](const auto& log) { return log.first == level; });
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, TransportSetupIs19200BaudNoParity)
{
    const auto setup = SubaruUnisiaJecsM32rBootModeProgramExecutor{}.transport_setup(program_plan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 19200);               // write_mem() :364
    EXPECT_EQ(setup->parity, KlineParity::None); // write_mem() :362
    EXPECT_FALSE(setup->iso14230);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, BeforeConfigureResetsThenClearsTheHeader)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(
        SubaruUnisiaJecsM32rBootModeProgramExecutor{}.before_transport_configure(transport, clock, cancellation),
        IsOk());
    EXPECT_EQ(transport.lifecycle_calls_, std::vector<std::string>{"reset_connection"}); // write_mem() :361
    EXPECT_EQ(transport.header_mode_calls_, std::vector<bool>{false});
}

class ProgramsTheWholeRom
    : public ::testing::TestWithParam<std::tuple<std::string_view, std::string_view, std::uint32_t>>
{
};

TEST_P(ProgramsTheWholeRom, ErasesThenWritesEveryBlock)
{
    const auto [protocol, mcu, size] = GetParam();
    const FlashPlan plan = program_plan(protocol, mcu, size);
    const std::uint32_t blocks = size / 0x80;
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    script_blocks(transport, plan, blocks - 1);
    transport.exchange(block_request(plan, blocks - 1, true), reply({0xef, 0x52}));
    RunContext context;

    ASSERT_THAT(run(plan, transport, context), IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_, (std::vector{Line::EnableProgrammingVoltageLine, Line::DisableLecLines}));
    EXPECT_EQ(transport.programming_voltage_line_write_index_, std::optional<std::size_t>(0));
    // AF 31 settle 500, one empty start poll 500, one empty done poll 1000,
    // post-erase 1000, then 10 ms after every AF 61 (:370-465, :539).
    EXPECT_EQ(context.clock.elapsed(), 500ms + 500ms + 1000ms + 1000ms + 10ms * (blocks - 1));
    EXPECT_EQ(std::ranges::count(transport.operation_trace_, Op::Read10), 4);
    EXPECT_EQ(context.events.progress_calls.back(),
              (std::pair<int, int>{static_cast<int>(blocks), static_cast<int>(blocks)}));
    EXPECT_FALSE(logged(context, LogLevel::Warning));
}

INSTANTIATE_TEST_SUITE_P(BothBootmodeRoms, ProgramsTheWholeRom,
                         ::testing::Values(std::make_tuple("sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000U),
                                           std::make_tuple("sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000U)));

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, SilenceAfterTheFinalBlockSucceedsWithAWarning)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    script_blocks(transport, plan, 1023);
    transport.expectWrite(block_request(plan, 1023, true));
    transport.queue_no_frame();
    RunContext context;
    ASSERT_THAT(run(plan, transport, context), IsOk());
    EXPECT_TRUE(logged(context, LogLevel::Warning));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, ABadFinalBlockReplyFails)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    script_blocks(transport, plan, 1023);
    transport.exchange(block_request(plan, 1023, true), reply({0xef, 0x5c}));
    RunContext context;
    const auto result = run(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::BadResponse));
    EXPECT_THAT(result.error().detail, ::testing::HasSubstr("checksum error"));
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, EraseStartExhaustionFails)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf, 0x31}));
    for (int round = 0; round < 20; ++round)
    {
        transport.queue_no_frame();
    }
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(context.clock.elapsed(), 500ms + 20 * 500ms);
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, EraseDoneExhaustionFailsInsteadOfProgramming)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x42}));
    for (int round = 0; round < 20; ++round)
    {
        transport.queue_no_frame();
    }
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 1U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, AWrongEraseFrameFailsAndNamesTheStatus)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x48}));
    RunContext context;
    const auto result = run(plan, transport, context);
    ASSERT_THAT(result, IsErr(ErrorKind::BadResponse));
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
        const FlashPlan plan = program_plan();
        ScriptedKlineFlashTransport transport;
        script_erase(transport);
        transport.exchange(block_request(plan, 0, false), reply({0xef, status}));
        RunContext context;
        const auto result = run(plan, transport, context);
        ASSERT_THAT(result, IsErr(ErrorKind::BadResponse));
        EXPECT_THAT(result.error().detail, ::testing::HasSubstr(std::string(text)));
    }
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, SilenceAfterANonFinalBlockTimesOut)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    transport.expectWrite(block_request(plan, 0, false));
    transport.queue_no_frame();
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Timeout));
    EXPECT_EQ(transport.writesConsumed(), 2U);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancelledBeforeVoltageNeverRaisesIt)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    RunContext context;
    context.cancellation.set_cancelled(true);
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.control_line_trace_, std::vector{Line::DisableLecLines});
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CancelledMidProgrammingReportsCancelled)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    script_erase(transport);
    script_blocks(transport, plan, 3);
    RunContext context;
    context.cancellation.set_predicate([&transport] { return transport.writesConsumed() >= 3; });
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Cancelled));
    EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines);
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, CleanupFailureNeverReplacesAnEarlierError)
{
    const FlashPlan plan = program_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(request({0xaf, 0x31}));
    transport.queueRead(reply({0xef, 0x48}));
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::BadResponse));
}

TEST(SubaruUnisiaJecsM32rBootModeProgramExecutor, RejectsAKernelPlan)
{
    auto plan = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(
        FlashOperation::Write, "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", bytes::Bytes(0x80, 0x00));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeProgramExecutor{}.transport_setup(*plan), IsErr(ErrorKind::InvalidConfig));
}
} // namespace
} // namespace fastecu::flash
