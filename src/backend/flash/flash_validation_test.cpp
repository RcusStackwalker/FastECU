// src/backend/flash/flash_validation_test.cpp
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
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

// The "at least one confirmation" floor was removed (Step 5 tail, wave 0):
// families whose read path prompts for nothing, like Mitsu Colt CAN, must be
// able to build a plan with zero confirmations. This is no longer rejected.
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

namespace
{

// Minimal fields for a family that uploads no kernel and prompts for
// nothing -- the shape the Mitsu Colt CAN read plan produces.
fastecu::flash::FlashPlanFields kernellessReadFields()
{
    using namespace fastecu::flash;
    FlashPlanFields fields;
    fields.operation = FlashOperation::Read;
    fields.family = FlashFamily::MitsuColtM32rCan;
    fields.transport = TransportKind::CanIso15765;
    fields.target_id = "mitsu_ecu_m32r_can";
    fields.mcu_name = "M32R_384KB_1block";
    fields.transfer_region = MemoryRegion{0x00008000, 0x00058000};
    fields.kernel = std::nullopt;
    fields.family_plan = MitsuColtM32rCanPlan{0x7e0, 0x7e8, 500000, false, false, 0x81};
    return fields;
}

} // namespace

TEST(FlashValidation, AcceptsAPlanWithNoKernelAndNoConfirmations)
{
    const auto plan = fastecu::flash::validate_and_build(kernellessReadFields());

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_TRUE(plan->confirmations().empty());
    EXPECT_EQ(plan->experimental_family_id(), "MitsuColtM32rCan");
}

TEST(FlashValidation, RejectsAPresentKernelWithNoBytes)
{
    auto fields = kernellessReadFields();
    fields.kernel = fastecu::flash::KernelImage{"colt", 0x800000, {}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("kernel bytes"));
}

TEST(FlashValidation, RejectsAPresentKernelWithNoId)
{
    auto fields = kernellessReadFields();
    fields.kernel = fastecu::flash::KernelImage{"", 0x800000, {0x01, 0x02}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("kernel id"));
}

TEST(FlashValidation, RejectsAColtPlanOnAKlineTransport)
{
    auto fields = kernellessReadFields();
    fields.transport = fastecu::flash::TransportKind::Kline;

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("does not match transport kind"));
}

TEST(FlashValidation, StillRejectsDuplicateConfirmationIds)
{
    using fastecu::flash::ConfirmationSpec;
    auto fields = kernellessReadFields();
    fields.operation = fastecu::flash::FlashOperation::Write;
    fields.image = bytes::Bytes(0x80000, 0x00);
    fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::EraseTrigger, {}},
                            ConfirmationSpec{ConfirmationSpec::Id::EraseTrigger, {}}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("duplicate confirmation id"));
}
