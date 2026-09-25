#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/flash_validation.h"
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

// 200 bytes: two 128-byte chunks once padded.
bytes::Bytes kernel_file()
{
    bytes::Bytes kernel(200);
    for (std::size_t i = 0; i < kernel.size(); ++i)
    {
        kernel[i] = static_cast<bytes::Byte>(i + 1);
    }
    return kernel;
}

FlashPlan kernel_plan()
{
    auto plan = build_subaru_unisia_jecs_m32r_bootmode_kernel_plan(
        FlashOperation::Write, "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", kernel_file());
    EXPECT_THAT(plan, IsOk());
    return std::move(*plan);
}

bytes::Bytes chunk(const FlashPlan& plan, std::size_t index)
{
    const auto begin = plan.image()->begin() + static_cast<std::ptrdiff_t>(index * 0x80);
    return bytes::Bytes(begin, begin + 0x80);
}

struct RunContext
{
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
};

Result<FlashExecutionResult> run(const FlashPlan& plan, ScriptedKlineFlashTransport& transport, RunContext& context)
{
    return SubaruUnisiaJecsM32rBootModeKernelExecutor{}.execute(plan, transport, context.clock, context.cancellation,
                                                                context.events);
}

void script_upload(ScriptedKlineFlashTransport& transport, const FlashPlan& plan)
{
    auto section = transport.section("kernel chunks");
    transport.expectWrite(chunk(plan, 0));
    transport.expectWrite(chunk(plan, 1));
    transport.queue_no_frame(); // the discarded settle read
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, TransportSetupIs39063BaudEvenParity)
{
    const auto setup = SubaruUnisiaJecsM32rBootModeKernelExecutor{}.transport_setup(kernel_plan());
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 39063);               // execute() :58
    EXPECT_EQ(setup->parity, KlineParity::Even); // execute() :57
    EXPECT_FALSE(setup->iso14230);               // execute() :53
    EXPECT_EQ(setup->tester_id, 0xf0);           // execute() :59
    EXPECT_EQ(setup->target_id, 0x10);           // execute() :60
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, BeforeConfigureResetsThenClearsTheHeader)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruUnisiaJecsM32rBootModeKernelExecutor{}.before_transport_configure(transport, clock, cancellation),
                IsOk());
    EXPECT_EQ(transport.lifecycle_calls_, std::vector<std::string>{"reset_connection"}); // execute() :52
    EXPECT_EQ(transport.header_mode_calls_, std::vector<bool>{false});
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, UploadsEveryChunkUnderBootModeLines)
{
    const FlashPlan plan = kernel_plan();
    ScriptedKlineFlashTransport transport;
    script_upload(transport, plan);
    RunContext context;
    ASSERT_THAT(run(plan, transport, context), IsOk());
    EXPECT_TRUE(transport.scriptConsumed());
    EXPECT_EQ(transport.control_line_trace_, (std::vector{Line::EnableBootModeLines, Line::DisableLecLines}));
    EXPECT_EQ(transport.read_timeouts_, std::vector<std::chrono::milliseconds>{200ms}); // :339
    EXPECT_EQ(context.clock.elapsed(), 500ms);                                          // :338
    EXPECT_EQ(context.events.progress_calls, (std::vector<std::pair<int, int>>{{1, 2}, {2, 2}}));
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, SettleReplyIsLoggedAndIgnored)
{
    const FlashPlan plan = kernel_plan();
    ScriptedKlineFlashTransport transport;
    transport.expectWrite(chunk(plan, 0));
    transport.expectWrite(chunk(plan, 1));
    transport.queueRead(bytes::Bytes{0x55, 0xaa});
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsOk());
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, APlanWithoutTheConfirmationTouchesNothing)
{
    auto plan = validate_and_build(FlashPlanFields{
        .operation = FlashOperation::Write,
        .family = FlashFamily::SubaruUnisiaJecsM32rBootModeKernel,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_unisia_jecs_20_bootmode",
        .mcu_name = "M32R_128KB",
        .transfer_region = {0, 0x80},
        .erase_regions = {},
        .image = bytes::Bytes(0x80, 0x00),
        .kernel = std::nullopt,
        .family_plan =
            SubaruUnisiaJecsM32rBootModeKernelPlan{.initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10},
        .confirmations = {},
    });
    ASSERT_THAT(plan, IsOk());
    ScriptedKlineFlashTransport transport;
    RunContext context;
    EXPECT_THAT(run(*plan, transport, context), IsErr(ErrorKind::InvalidConfig));
    EXPECT_TRUE(transport.control_line_trace_.empty());
    EXPECT_EQ(transport.writesConsumed(), 0U);
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, RejectsAProgramPlan)
{
    auto plan = build_subaru_unisia_jecs_m32r_bootmode_program_plan(
        FlashOperation::Write, "sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", bytes::Bytes(0x20000, 0x00));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeKernelExecutor{}.transport_setup(*plan), IsErr(ErrorKind::InvalidConfig));
}

// Check 1 precedes the lines, checks 2 and 3 precede each chunk.
TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, CancellationAtEachCheckpointDropsTheLines)
{
    for (const std::size_t check : {1U, 2U, 3U})
    {
        const FlashPlan plan = kernel_plan();
        ScriptedKlineFlashTransport transport;
        script_upload(transport, plan);
        RunContext context;
        context.cancellation.cancel_on_check(check);
        EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Cancelled)) << "check " << check;
        ASSERT_FALSE(transport.control_line_trace_.empty());
        EXPECT_EQ(transport.control_line_trace_.back(), Line::DisableLecLines) << "check " << check;
        // Check 1 stops before any chunk; check N >= 2 stops before chunk N-2.
        EXPECT_EQ(transport.writesConsumed(), check >= 2 ? check - 2 : 0U) << "check " << check;
    }
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, LineFailureStillDropsTheLinesAndKeepsItsError)
{
    const FlashPlan plan = kernel_plan();
    ScriptedKlineFlashTransport transport;
    transport.enable_boot_mode_lines_result_ = fail(ErrorKind::Internal, "lines");
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Internal));
    EXPECT_EQ(transport.control_line_trace_, (std::vector{Line::EnableBootModeLines, Line::DisableLecLines}));
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, CleanupFailureFailsAnOtherwiseGoodUpload)
{
    const FlashPlan plan = kernel_plan();
    ScriptedKlineFlashTransport transport;
    script_upload(transport, plan);
    transport.disable_lec_lines_result_ = fail(ErrorKind::Disconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(run(plan, transport, context), IsErr(ErrorKind::Disconnected));
}
} // namespace
} // namespace fastecu::flash
