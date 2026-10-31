#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_types.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/flash_validation.h"
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
using fastecu::testing::IsOk;
using ::testing::IsEmpty;
using ::testing::Not;
using Line = ScriptedKlineFlashTransport::ControlLineAction;

// 200 bytes: two 128-byte chunks once padded.
bytes::Bytes KernelFile()
{
    bytes::Bytes kernel(200);
    for (std::size_t i = 0; i < kernel.size(); ++i)
    {
        kernel[i] = static_cast<bytes::Byte>(i + 1);
    }
    return kernel;
}

Result<FlashPlan> KernelPlan()
{
    return BuildSubaruUnisiaJecsM32rBootmodeKernelPlan(FlashOperation::kWrite, "sub_ecu_unisia_jecs_20_bootmode",
                                                       "M32R_128KB", KernelFile());
}

bytes::Bytes Chunk(const FlashPlan& plan, std::size_t index)
{
    const auto begin = plan.ImageOrEmpty().begin() + static_cast<std::ptrdiff_t>(index * 0x80);
    return bytes::Bytes(begin, begin + 0x80);
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
    return SubaruUnisiaJecsM32rBootModeKernelExecutor{}.Execute(plan, transport, context.clock, context.cancellation,
                                                                context.events);
}

void ScriptUpload(ScriptedKlineFlashTransport& transport, const FlashPlan& plan)
{
    auto section = transport.Section("kernel chunks");
    transport.ExpectWrite(Chunk(plan, 0));
    transport.ExpectWrite(Chunk(plan, 1));
    transport.QueueNoFrame(); // the discarded settle read
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, TransportSetupIs39063BaudEvenParity)
{
    auto built_plan_1 = KernelPlan();
    ASSERT_THAT(built_plan_1, fastecu::testing::IsOk());
    const auto setup = SubaruUnisiaJecsM32rBootModeKernelExecutor{}.TransportSetup(std::move(*built_plan_1));
    ASSERT_THAT(setup, IsOk());
    EXPECT_EQ(setup->baud, 39063);
    EXPECT_EQ(setup->parity, KlineParity::kEven);
    EXPECT_FALSE(setup->iso14230);
    EXPECT_EQ(setup->tester_id, 0xf0);
    EXPECT_EQ(setup->target_id, 0x10);
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, BeforeConfigureResetsThenClearsTheHeader)
{
    ScriptedKlineFlashTransport transport;
    FakeClock clock;
    FakeCancellationToken cancellation;
    ASSERT_THAT(SubaruUnisiaJecsM32rBootModeKernelExecutor{}.BeforeTransportConfigure(transport, clock, cancellation),
                IsOk());
    EXPECT_EQ(transport.lifecycle_calls, std::vector<std::string>{"reset_connection"});
    EXPECT_EQ(transport.header_mode_calls, std::vector<bool>{false});
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, UploadsEveryChunkUnderBootModeLines)
{
    auto built_plan_2 = KernelPlan();
    ASSERT_THAT(built_plan_2, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_2);
    ScriptedKlineFlashTransport transport;
    ScriptUpload(transport, plan);
    RunContext context;
    ASSERT_THAT(RunScripted(plan, transport, context), IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_EQ(transport.control_line_trace, (std::vector{Line::kEnableBootModeLines, Line::kDisableLecLines}));
    EXPECT_EQ(transport.read_timeouts, std::vector<std::chrono::milliseconds>{200ms});
    EXPECT_EQ(context.clock.Elapsed(), 500ms);
    EXPECT_EQ(context.events.progress_calls, (std::vector<std::pair<int, int>>{{1, 2}, {2, 2}}));
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, SettleReplyIsLoggedAndIgnored)
{
    auto built_plan_3 = KernelPlan();
    ASSERT_THAT(built_plan_3, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_3);
    ScriptedKlineFlashTransport transport;
    transport.ExpectWrite(Chunk(plan, 0));
    transport.ExpectWrite(Chunk(plan, 1));
    transport.QueueRead(bytes::Bytes{0x55, 0xaa});
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsOk());
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, APlanWithoutTheConfirmationTouchesNothing)
{
    auto plan = ValidateAndBuild(FlashPlanFields{
        .operation = FlashOperation::kWrite,
        .family = FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel,
        .transport = TransportKind::kKline,
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
    EXPECT_THAT(RunScripted(*plan, transport, context), IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(transport.control_line_trace, IsEmpty());
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, RejectsAProgramPlan)
{
    auto plan = BuildSubaruUnisiaJecsM32rBootmodeProgramPlan(FlashOperation::kWrite, "sub_ecu_unisia_jecs_20_bootmode",
                                                             "M32R_128KB", bytes::Bytes(0x20000, 0x00));
    ASSERT_THAT(plan, IsOk());
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeKernelExecutor{}.TransportSetup(*plan), IsErr(ErrorKind::kInvalidConfig));
}

// Check 1 is plan admission (see CancelledAtEntryTouchesNoControlLine);
// checks 2 and 3 precede each chunk.
TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, CancellationAtEachCheckpointDropsTheLines)
{
    for (const std::size_t check : {2U, 3U})
    {
        SCOPED_TRACE(::testing::PrintToString(check));
        auto built_plan_4 = KernelPlan();
        ASSERT_THAT(built_plan_4, fastecu::testing::IsOk());
        const FlashPlan plan = std::move(*built_plan_4);
        ScriptedKlineFlashTransport transport;
        ScriptUpload(transport, plan);
        RunContext context;
        context.cancellation.CancelOnCheck(check);
        EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kCancelled)) << "check " << check;
        ASSERT_THAT(transport.control_line_trace, Not(IsEmpty()));
        EXPECT_EQ(transport.control_line_trace.back(), Line::kDisableLecLines) << "check " << check;
        // Check N stops before chunk N-2.
        EXPECT_EQ(transport.WritesConsumed(), check - 2) << "check " << check;
    }
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, LineFailureStillDropsTheLinesAndKeepsItsError)
{
    auto built_plan_5 = KernelPlan();
    ASSERT_THAT(built_plan_5, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_5);
    ScriptedKlineFlashTransport transport;
    transport.enable_boot_mode_lines_result = Fail(ErrorKind::kInternal, "lines");
    transport.disable_lec_lines_result = Fail(ErrorKind::kDisconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kInternal));
    EXPECT_EQ(transport.control_line_trace, (std::vector{Line::kEnableBootModeLines, Line::kDisableLecLines}));
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, CleanupFailureFailsAnOtherwiseGoodUpload)
{
    auto built_plan_6 = KernelPlan();
    ASSERT_THAT(built_plan_6, fastecu::testing::IsOk());
    const FlashPlan plan = std::move(*built_plan_6);
    ScriptedKlineFlashTransport transport;
    ScriptUpload(transport, plan);
    transport.disable_lec_lines_result = Fail(ErrorKind::kDisconnected, "cleanup");
    RunContext context;
    EXPECT_THAT(RunScripted(plan, transport, context), IsErr(ErrorKind::kDisconnected));
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, CancelledAtEntryTouchesNoControlLine)
{
    auto plan = KernelPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptedKlineFlashTransport transport;
    RunContext context;
    context.cancellation.CancelOnCheck(1);

    EXPECT_THAT(RunScripted(*plan, transport, context),
                fastecu::testing::IsErrWith(ErrorKind::kCancelled, ::testing::Eq("cancelled before setup")));
    EXPECT_THAT(transport.control_line_trace, IsEmpty());
    EXPECT_EQ(transport.WritesConsumed(), 0U);
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, EveryUploadTransportFaultStillDropsTheLines)
{
    auto plan = KernelPlan();
    ASSERT_THAT(plan, IsOk());
    for (const auto& [op, count] :
         std::to_array<std::pair<KlineFaultOp, std::size_t>>({{KlineFaultOp::kWrite, 2}, {KlineFaultOp::kRead, 1}}))
    {
        SCOPED_TRACE(::testing::PrintToString(static_cast<int>(op)));
        for (std::size_t k = 1; k <= count; ++k)
        {
            SCOPED_TRACE(::testing::PrintToString(std::pair{static_cast<int>(op), k}));
            FaultingKlineFlashTransport transport;
            transport.op = op;
            transport.fail_at = k;
            ScriptUpload(transport, *plan);
            RunContext context;
            EXPECT_THAT(RunScripted(*plan, transport, context),
                        fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("injected")));
            EXPECT_EQ(transport.writes_at_fault, std::optional<std::size_t>(transport.WritesConsumed()));
            EXPECT_EQ(transport.control_line_trace, (std::vector{Line::kEnableBootModeLines, Line::kDisableLecLines}));
        }
    }
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, ASettleSleepFailureSkipsTheReadAndDropsTheLines)
{
    auto plan = KernelPlan();
    ASSERT_THAT(plan, IsOk());
    ScriptedKlineFlashTransport transport;
    ScriptUpload(transport, *plan);
    FaultingClock clock;
    clock.fail_at = 1;
    FakeCancellationToken cancellation;
    RecordingEventSink events;
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeKernelExecutor{}.Execute(*plan, transport, clock, cancellation, events),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("sleep failed")));
    EXPECT_EQ(transport.WritesConsumed(), 2U);
    EXPECT_THAT(transport.read_timeouts, IsEmpty());
    EXPECT_EQ(transport.control_line_trace, (std::vector{Line::kEnableBootModeLines, Line::kDisableLecLines}));
}

TEST(SubaruUnisiaJecsM32rBootModeKernelExecutor, AResetFailureSkipsTheHeader)
{
    ScriptedKlineFlashTransport transport;
    transport.reset_result = Fail(ErrorKind::kDisconnected, "reset");
    FakeClock clock;
    FakeCancellationToken cancellation;
    EXPECT_THAT(SubaruUnisiaJecsM32rBootModeKernelExecutor{}.BeforeTransportConfigure(transport, clock, cancellation),
                fastecu::testing::IsErrWith(ErrorKind::kDisconnected, ::testing::Eq("reset")));
    EXPECT_THAT(transport.header_mode_calls, IsEmpty());
}

} // namespace
} // namespace fastecu::flash
