// src/backend/flash/flash_plan_test.cpp
#include "src/backend/flash/flash_plan.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

// validate_and_build does not exist until Task 3; this test calls the
// private-construction seam directly through a same-namespace helper so
// FlashPlan's public surface can be exercised before the real builder lands.
Result<FlashPlan> make_test_plan(FlashPlanFields fields)
{
    return FlashPlan::for_test(std::move(fields));
}

FlashPlanFields minimal_read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromKline,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0xf000, .length = 0x1000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff2000, .bytes = {0x01}},
        .family_plan = DensoSh705xEepromKlinePlan{
            .mode = EepromReadMode::Mode2,
            .security = DensoSecurityVariant::Stock,
            .tester_id = 0xf0,
            .target_id = 0x10,
            .initial_baud = 4800,
            .kernel_baud = 15625,
        },
        .confirmations = {
            ConfirmationSpec{.id = ConfirmationSpec::Id::BeginEepromRead},
            ConfirmationSpec{.id = ConfirmationSpec::Id::InspectEepromBytes},
        },
    };
}

TEST(FlashPlanTest, AccessorsReturnConstructedFields)
{
    auto plan = make_test_plan(minimal_read_fields());
    ASSERT_TRUE(plan.has_value());

    EXPECT_EQ(plan->operation(), FlashOperation::Read);
    EXPECT_EQ(plan->family(), FlashFamily::DensoSh705xEepromKline);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);
    EXPECT_EQ(plan->target_id(), "sub_ecu_eeprom_denso_sh7055_kline");
    EXPECT_EQ(plan->mcu_name(), "SH7055");
    EXPECT_EQ(plan->transfer_region().start, 0xf000u);
    EXPECT_TRUE(plan->erase_regions().empty());
    EXPECT_FALSE(plan->image().has_value());
    EXPECT_EQ(plan->kernel().load_address, 0xffff2000u);
    EXPECT_EQ(plan->confirmations().size(), 2u);
    EXPECT_EQ(plan->total_transfer_bytes(), 0x1000u);
    EXPECT_EQ(plan->experimental_family_id(), "DensoSh705xEepromKline");
}

TEST(FlashPlanTest, ExperimentalFamilyIdMatchesCanFamily)
{
    auto fields = minimal_read_fields();
    fields.family = FlashFamily::DensoSh705xEepromCan;
    auto plan = make_test_plan(std::move(fields));

    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->experimental_family_id(), "DensoSh705xEepromCan");
}

TEST(FlashPlanTest, KernelBytesAreOwnedIndependentlyOfSourceVector)
{
    auto fields = minimal_read_fields();
    bytes::Bytes source{0x11, 0x22};
    fields.kernel.bytes = source;

    auto plan = make_test_plan(std::move(fields));
    ASSERT_TRUE(plan.has_value());

    source[0] = 0xee;

    EXPECT_EQ(plan->kernel().bytes[0], 0x11);
}

} // namespace
} // namespace fastecu::flash
