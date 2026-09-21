#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
TEST(SubaruHitachiSh7058Plan, ReadAndWriteHaveDistinctTransportAndExactGeometry)
{
    auto read = build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                 std::nullopt);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->transport(), TransportKind::Kline);
    EXPECT_EQ(read->transfer_region().start, 0x100000U);
    EXPECT_EQ(read->transfer_region().length, 0x100000U);
    auto write = build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                  bytes::Bytes(0x100000));
    ASSERT_TRUE(write.has_value());
    EXPECT_EQ(write->transport(), TransportKind::CanIso15765);
    EXPECT_EQ(write->transfer_region().start, 0U);
    EXPECT_EQ(write->transfer_region().length, 0x100000U);
}

TEST(SubaruHitachiSh7058Plan, RejectsUnsafeOperationsAndNearMisses)
{
    EXPECT_FALSE(build_subaru_hitachi_sh7058_plan(FlashOperation::TestWrite, "sub_ecu_hitachi_sh7058_can",
                                                  "SH7058_1block", std::nullopt)
                     .has_value());
    EXPECT_FALSE(build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can_extra",
                                                  "SH7058_1block", std::nullopt)
                     .has_value());
    EXPECT_FALSE(
        build_subaru_hitachi_sh7058_plan(FlashOperation::Read, "sub_ecu_hitachi_sh7058_can", "SH7058", std::nullopt)
            .has_value());
    EXPECT_FALSE(build_subaru_hitachi_sh7058_plan(FlashOperation::Write, "sub_ecu_hitachi_sh7058_can", "SH7058_1block",
                                                  bytes::Bytes(0xFFFFF))
                     .has_value());
}

TEST(SubaruHitachiSh7058Plan, RejectsForgedTransportAndWireParameters)
{
    FlashPlanFields fields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruHitachiSh7058,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_hitachi_sh7058_can",
        .mcu_name = "SH7058_1block",
        .transfer_region = {0x100000, 0x100000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruHitachiSh7058KlinePlan{.initial_baud = 9600},
        .confirmations = {},
    };
    auto forged = validate_and_build(fields);
    ASSERT_TRUE(forged.has_value());
    EXPECT_FALSE(validate_subaru_hitachi_sh7058_plan(*forged).has_value());
    fields.transport = TransportKind::CanIso15765;
    EXPECT_FALSE(validate_and_build(fields).has_value());
}
} // namespace fastecu::flash
