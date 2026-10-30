#include "src/backend/flash/ecu/bootloader_operation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <utility>

#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"
#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{

struct Ctx
{
    IEventSink& events;
};

// Plan validation keeps TestWrite from reaching any family's Run, so only a
// direct call shows the operation's own last-line refusal.
TEST(RunBootloaderOperation, RefusesTestWriteAfterConnectingWithoutErasingOrWriting)
{
    FlashPlanFields fields;
    fields.operation = FlashOperation::kTestWrite;
    fields.family = FlashFamily::kSubaruHitachiM32rCan;
    fields.transport = TransportKind::kCanIso15765;
    fields.target_id = "sub_ecu_hitachi_m32r_can";
    fields.mcu_name = "M32R_512kB";
    fields.transfer_region = MemoryRegion{0, 0x80000};
    fields.erase_regions = {MemoryRegion{0, 0x80000}};
    fields.image = bytes::Bytes(0x80000, 0x00);
    fields.family_plan = SubaruHitachiM32rCanPlan{0x7e0, 0x7e8, 500000, false};
    const Result<FlashPlan> plan = ValidateAndBuild(std::move(fields));
    ASSERT_THAT(plan, testing::IsOk());
    RecordingEventSink events;
    Ctx ctx{events};
    int writes = 0;

    const auto result = RunBootloaderOperation(
        *plan, "Unit X", ctx, [](Ctx&) { return Status{}; },
        [](Ctx&, PhaseReporter&) { return Result<bytes::Bytes>{}; },
        [&](Ctx&)
        {
            ++writes;
            return Status{};
        },
        [&](Ctx&, bytes::ByteView, PhaseReporter&)
        {
            ++writes;
            return Status{};
        });

    EXPECT_THAT(result, testing::IsErrWith(ErrorKind::kUnsupported,
                                           ::testing::StrEq("test_write is not supported by the Unit X CAN family")));
    EXPECT_EQ(writes, 0);
    EXPECT_THAT(events.notices, ::testing::IsEmpty());
}

} // namespace
} // namespace fastecu::flash
