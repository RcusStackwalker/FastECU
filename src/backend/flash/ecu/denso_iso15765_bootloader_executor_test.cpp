#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>

#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/testing/can_executor_run.h"
#include "src/backend/flash/ecu/testing/denso_can_frames.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/testing/result_matchers.h"

// What the four family suites leave unpinned about the Denso ISO-15765
// bootloader dialect: which family takes which branch of a dialect choice, and
// the log wording and block index each branch produces. Each family suite pins
// its wire transcript; this file pins the choices between them.
namespace fastecu::flash
{
namespace
{

using fastecu::LogLevel;
using fastecu::flash::testing::ExecutorRun;
using fastecu::flash::testing::Request;
using fastecu::flash::testing::Response;
using ::testing::Contains;
using ::testing::Pair;

template <typename Plan> constexpr const DensoIso15765BootloaderProfile& ProfileOf()
{
    return DensoIso15765BootloaderProfileOf<Plan>::kValue;
}

// Only SH72543 diesel steps over a wrong bench session reply; only 1N83M 4M
// steps over a bad dump setup.
TEST(DensoIso15765BootloaderDialect, ReplyStrictnessIsChosenPerFamily)
{
    EXPECT_EQ(ProfileOf<SubaruDenso1n83m_1_5mCanPlan>().bench.session_reply, DensoBenchSessionReply::kFatal);
    EXPECT_EQ(ProfileOf<SubaruDenso1n83m_4mCanPlan>().bench.session_reply, DensoBenchSessionReply::kFatal);
    EXPECT_EQ(ProfileOf<SubaruDensoSh72531CanPlan>().bench.session_reply, DensoBenchSessionReply::kFatal);
    EXPECT_EQ(ProfileOf<SubaruDensoSh72543CanDieselPlan>().bench.session_reply, DensoBenchSessionReply::kTolerantProbe);

    EXPECT_EQ(ProfileOf<SubaruDenso1n83m_1_5mCanPlan>().read_setup_reply, DensoReadSetupReply::kFatal);
    EXPECT_EQ(ProfileOf<SubaruDenso1n83m_4mCanPlan>().read_setup_reply, DensoReadSetupReply::kTolerantReply);
    EXPECT_EQ(ProfileOf<SubaruDensoSh72531CanPlan>().read_setup_reply, DensoReadSetupReply::kFatal);
    EXPECT_EQ(ProfileOf<SubaruDensoSh72543CanDieselPlan>().read_setup_reply, DensoReadSetupReply::kFatal);
}

// Only SH72543 diesel words its region lines as start-and-length and flashes block 0.
TEST(DensoIso15765BootloaderDialect, RegionLogAndBlockIndexAreChosenPerFamily)
{
    EXPECT_EQ(ProfileOf<SubaruDenso1n83m_1_5mCanPlan>().region_log, DensoRegionLog::kDumpAndBlock);
    EXPECT_EQ(ProfileOf<SubaruDenso1n83m_4mCanPlan>().region_log, DensoRegionLog::kDumpAndBlock);
    EXPECT_EQ(ProfileOf<SubaruDensoSh72531CanPlan>().region_log, DensoRegionLog::kDumpAndBlock);
    EXPECT_EQ(ProfileOf<SubaruDensoSh72543CanDieselPlan>().region_log, DensoRegionLog::kStartAndLength);

    EXPECT_EQ(ProfileOf<SubaruDenso1n83m_1_5mCanPlan>().write_block_index, 1);
    EXPECT_EQ(ProfileOf<SubaruDenso1n83m_4mCanPlan>().write_block_index, 1);
    EXPECT_EQ(ProfileOf<SubaruDensoSh72531CanPlan>().write_block_index, 1);
    EXPECT_EQ(ProfileOf<SubaruDensoSh72543CanDieselPlan>().write_block_index, 0);
}

// SH72531's bench connect, as its own suite scripts it.
void ScriptSh72531BenchConnect(ScriptedCanFlashTransport& t)
{
    fastecu::flash::testing::ScriptDensoPreliminaries(t, 0xFF, 0);
    t.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    t.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    t.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    t.Exchange(Request({0x10, 0x42}), Response({0x7F, 0x10, 0x78}));
    t.QueueRead(Response({0x50, 0x42}));
}

TEST(DensoIso15765BootloaderDialect, DumpAndBlockReadLogsTheLegacyDumpSetupLine)
{
    ScriptedCanFlashTransport transport;
    ScriptSh72531BenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x13, 0x7F, 0x00}),
                       Response({0x7F, 0x34, 0x31}));

    const auto plan =
        BuildSubaruDensoSh72531CanPlan(FlashOperation::kRead, "sub_ecu_denso_sh72531_can", "SH72531", std::nullopt);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto run = ExecutorRun<SubaruDensoSh72531CanExecutor>(*plan, transport);

    EXPECT_THAT(run.result, fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_THAT(run.events.logs, Contains(Pair(LogLevel::kInfo, "Setting dump start & length")));
}

// The write stops at its first chunk, after both block lines have been logged.
TEST(DensoIso15765BootloaderDialect, DumpAndBlockWriteLogsBlockOneAndItsAddress)
{
    ScriptedCanFlashTransport transport;
    ScriptSh72531BenchConnect(transport);
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x13, 0x7F, 0x00}),
                       Response({0x74, 0x20, 0x01, 0x05}));
    transport.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
    transport.QueueRead(Response({0x71, 0x01, 0x02}));

    const auto plan = BuildSubaruDensoSh72531CanPlan(FlashOperation::kWrite, "sub_ecu_denso_sh72531_can", "SH72531",
                                                     fastecu::flash::testing::PatternedRom(0x140000));
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto run = ExecutorRun<SubaruDensoSh72531CanExecutor>(*plan, transport);

    // The first unscripted 0xB6 chunk write is the scripted transport's kInternal.
    EXPECT_THAT(run.result, fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_THAT(run.events.logs, Contains(Pair(LogLevel::kInfo, "Blocks to flash: 1,  (total: 1)")));
    EXPECT_THAT(run.events.logs, Contains(Pair(LogLevel::kInfo, "Flash block addr: 0x00008000 len: 0x00137F00")));
}

// SH72543 diesel words its write-block line as start-and-length, like its read
// setup, and flashes block 0. The write stops at its first chunk.
TEST(DensoIso15765BootloaderDialect, StartAndLengthWriteLogsBlockZeroAndItsRegion)
{
    ScriptedCanFlashTransport transport;
    fastecu::flash::testing::ScriptDensoPreliminaries(transport, 0xFF, 0, true);
    transport.Exchange(Request({0x10, 0x43}), Response({0x50, 0x43}));
    transport.Exchange(Request({0x27, 0x61}), Response({0x67, 0x61, 0x11, 0x22, 0x33, 0x44}));
    transport.Exchange(Request({0x27, 0x62, 0x35, 0xB6, 0x83, 0xBF}), Response({0x67, 0x62}));
    transport.Exchange(Request({0x10, 0x42}), Response({0x50, 0x42}));
    transport.Exchange(Request({0x34, 0x04, 0x44, 0x00, 0x00, 0x80, 0x00, 0x00, 0x1F, 0x7F, 0x00}),
                       Response({0x74, 0x20, 0x01, 0x05}));
    transport.Exchange(Request({0x31, 0x01, 0x02, 0x01, 0xFF, 0xFF, 0xFF, 0xFF}));
    transport.QueueRead(Response({0x71, 0x01, 0x02}));

    const auto plan = BuildSubaruDensoSh72543CanDieselPlan(FlashOperation::kWrite, "sub_ecu_denso_sh72543_can_diesel",
                                                           "SH72543d", fastecu::flash::testing::PatternedRom(0x200000));
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    const auto run = ExecutorRun<SubaruDensoSh72543CanDieselExecutor>(*plan, transport);

    // The first unscripted 0xB6 chunk write is the scripted transport's kInternal.
    EXPECT_THAT(run.result, fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_THAT(run.events.logs, Contains(Pair(LogLevel::kInfo, "Blocks to flash: 0,  (total: 1)")));
    EXPECT_THAT(run.events.logs, Contains(Pair(LogLevel::kInfo, "Setting flash start: 0x8000 and length: 0x1f7f00")));
    EXPECT_THAT(run.events.logs,
                ::testing::Not(Contains(Pair(LogLevel::kInfo, ::testing::StartsWith("Flash block addr")))));
}

} // namespace
} // namespace fastecu::flash
