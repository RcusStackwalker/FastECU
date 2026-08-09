// src/backend/flash/flash_validation_test.cpp
#include "src/backend/flash/flash_validation.h"

#include <gtest/gtest.h>

namespace fastecu::flash
{
namespace
{

FlashPlanFields valid_read_fields()
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

FlashPlanFields valid_can_read_fields()
{
    auto fields = valid_read_fields();
    fields.family = FlashFamily::DensoSh705xEepromCan;
    fields.transport = TransportKind::CanIso15765;
    fields.kernel = KernelImage{.id = "k", .load_address = 0xffff3000, .bytes = {0x01}};
    fields.family_plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::Mode2,
        .security = DensoSecurityVariant::Stock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };
    return fields;
}

TEST(FlashValidationTest, ValidReadFieldsProduceAPlan)
{
    auto plan = validate_and_build(valid_read_fields());
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->total_transfer_bytes(), 0x1000u);
}

TEST(FlashValidationTest, EmptyTargetIdIsRejected)
{
    auto fields = valid_read_fields();
    fields.target_id.clear();

    auto plan = validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, EmptyMcuNameIsRejected)
{
    auto fields = valid_read_fields();
    fields.mcu_name.clear();

    EXPECT_EQ(validate_and_build(std::move(fields)).error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, ZeroLengthTransferRegionIsRejected)
{
    auto fields = valid_read_fields();
    fields.transfer_region.length = 0;

    EXPECT_EQ(validate_and_build(std::move(fields)).error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, TransferRegionOverflowIsRejected)
{
    auto fields = valid_read_fields();
    fields.transfer_region = MemoryRegion{.start = 0xffffffff, .length = 0x10};

    EXPECT_EQ(validate_and_build(std::move(fields)).error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, ReadWithNonEmptyEraseRegionsIsRejected)
{
    auto fields = valid_read_fields();
    fields.erase_regions.push_back(MemoryRegion{.start = 0, .length = 4});

    EXPECT_EQ(validate_and_build(std::move(fields)).error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, ReadWithImagePresentIsRejected)
{
    auto fields = valid_read_fields();
    fields.image = bytes::Bytes{0x00};

    EXPECT_EQ(validate_and_build(std::move(fields)).error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, EmptyKernelIdIsRejected)
{
    auto fields = valid_read_fields();
    fields.kernel->id.clear();

    auto plan = validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, EmptyKernelBytesIsRejected)
{
    auto fields = valid_read_fields();
    fields.kernel->bytes.clear();

    auto plan = validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, MissingKernelIsRejectedForKlineFamilyByDefault)
{
    auto fields = valid_read_fields();
    fields.kernel = std::nullopt;

    auto plan = validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, MissingKernelIsRejectedForCanFamilyByDefault)
{
    auto fields = valid_can_read_fields();
    fields.kernel = std::nullopt;

    auto plan = validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, FamilyPlanTagMismatchWithTransportIsRejected)
{
    auto fields = valid_read_fields();
    // Kline transport but a Can family_plan variant.
    fields.family_plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::Mode2,
        .security = DensoSecurityVariant::Stock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    EXPECT_EQ(validate_and_build(std::move(fields)).error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, DuplicateConfirmationIdsAreRejected)
{
    auto fields = valid_read_fields();
    fields.confirmations.push_back(ConfirmationSpec{.id = ConfirmationSpec::Id::BeginEepromRead});

    EXPECT_EQ(validate_and_build(std::move(fields)).error().kind, ErrorKind::InvalidConfig);
}

TEST(FlashValidationTest, ZeroConfirmationsIsNowAccepted)
{
    auto fields = valid_read_fields();
    fields.confirmations.clear();

    auto plan = validate_and_build(std::move(fields));

    ASSERT_TRUE(plan.has_value());
    EXPECT_TRUE(plan->confirmations().empty());
}

} // namespace
} // namespace fastecu::flash
