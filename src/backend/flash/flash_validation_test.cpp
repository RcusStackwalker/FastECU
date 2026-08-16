// src/backend/flash/flash_validation_test.cpp
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>

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
        .family_plan =
            DensoSh705xEepromKlinePlan{
                .mode = EepromReadMode::Mode2,
                .security = DensoSecurityVariant::Stock,
                .tester_id = 0xf0,
                .target_id = 0x10,
                .initial_baud = 4800,
                .kernel_baud = 15625,
            },
        .confirmations =
            {
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

struct FamilyCase
{
    FlashFamily family;
    TransportKind transport;
    FamilyPlan family_plan;
    std::string_view id;
};

const std::array<FamilyCase, 7>& family_cases()
{
    static const std::array<FamilyCase, 7> cases{{
        {FlashFamily::DensoSh705xEepromKline, TransportKind::Kline,
         DensoSh705xEepromKlinePlan{.mode = EepromReadMode::Mode2,
                                    .security = DensoSecurityVariant::Stock,
                                    .tester_id = 0xf0,
                                    .target_id = 0x10,
                                    .initial_baud = 4800,
                                    .kernel_baud = 15625},
         "DensoSh705xEepromKline"},
        {FlashFamily::DensoSh705xEepromCan, TransportKind::CanIso15765,
         DensoSh705xEepromCanPlan{.mode = EepromReadMode::Mode2,
                                  .security = DensoSecurityVariant::Stock,
                                  .request_id = 0x7e0,
                                  .response_id = 0x7e8,
                                  .bitrate = 500000,
                                  .extended_id = false},
         "DensoSh705xEepromCan"},
        {FlashFamily::MitsuColtM32rCan, TransportKind::CanIso15765,
         MitsuColtM32rCanPlan{.request_id = 0x7e0,
                              .response_id = 0x7e8,
                              .bitrate = 500000,
                              .extended_id = false,
                              .use_vendor_challenge = false,
                              .session_id = 0x85},
         "MitsuColtM32rCan"},
        {FlashFamily::SubaruMitsuM32rKline, TransportKind::Kline,
         SubaruMitsuM32rKlinePlan{.tester_id = 0xf0,
                                  .target_id = 0x10,
                                  .initial_baud = 4800,
                                  .flash_baud = 62500,
                                  .chunk_size = 0x200,
                                  .unread_prefix_fill = 0xff},
         "SubaruMitsuM32rKline"},
        {FlashFamily::SubaruHitachiM32rKline, TransportKind::Kline,
         SubaruHitachiM32rKlinePlan{.session_mode = HitachiM32rKlineSessionMode::Normal,
                                    .tester_id = 0xf0,
                                    .target_id = 0x10,
                                    .initial_baud = 4800,
                                    .write_baud = 62500,
                                    .read_baud = 62500,
                                    .chunk_size = 0x200,
                                    .read_address_bias = 0},
         "SubaruHitachiM32rKline"},
        {FlashFamily::SubaruDensoMc68hc16y5_02, TransportKind::Kline,
         SubaruDensoMc68hc16y5_02Plan{.connect_baud = 9600,
                                      .kernel_baud = 9600,
                                      .encryption_xor = 0x55,
                                      .kernel_magic = 0x3941,
                                      .bootloader_ok = {0x4d, 0x00, 0xb3}},
         "SubaruDensoMc68hc16y5_02"},
        {FlashFamily::SubaruDensoSh7055_02, TransportKind::Kline,
         SubaruDensoSh7055_02Plan{.tester_id = 0xf0, .target_id = 0x10, .read_ecu_id = true}, "SubaruDensoSh7055_02"},
    }};
    return cases;
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

// A family that isn't the kernel-less Mitsu Colt CAN family must still carry
// a kernel -- the optional relaxation (Step 5 tail, wave 0) is scoped to
// MitsuColtM32rCan (family_requires_kernel_v's specialization), not a
// blanket relaxation for every family.
TEST(FlashValidationTest, MissingKernelIsRejectedForKlineFamilyByDefault)
{
    auto fields = valid_read_fields();
    fields.kernel = std::nullopt;

    auto plan = validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("kernel"));
}

TEST(FlashValidationTest, MissingKernelIsRejectedForCanFamilyByDefault)
{
    auto fields = valid_can_read_fields();
    fields.kernel = std::nullopt;

    auto plan = validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("kernel"));
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

TEST(FlashValidationTest, FamilyAndVariantMustMatchExhaustively)
{
    for (std::size_t declared_index = 0; declared_index < family_cases().size(); ++declared_index)
    {
        for (std::size_t variant_index = 0; variant_index < family_cases().size(); ++variant_index)
        {
            auto fields = valid_read_fields();
            const auto& declared = family_cases()[declared_index];
            fields.family = declared.family;
            fields.transport = declared.transport;
            fields.family_plan = family_cases()[variant_index].family_plan;

            const auto plan = validate_and_build(std::move(fields));

            EXPECT_EQ(plan.has_value(), declared_index == variant_index)
                << "declared index " << declared_index << ", variant index " << variant_index;
        }
    }
}

TEST(FlashValidationTest, ExperimentalFamilyIdsCoverEveryFamily)
{
    for (const auto& family_case : family_cases())
    {
        auto fields = valid_read_fields();
        fields.family = family_case.family;
        fields.transport = family_case.transport;
        fields.family_plan = family_case.family_plan;
        const auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        EXPECT_EQ(plan->experimental_family_id(), family_case.id);
    }
}

TEST(FlashValidationTest, Sh7055_02KlinePlanIsAccepted)
{
    auto fields = valid_read_fields();
    fields.family = FlashFamily::SubaruDensoSh7055_02;
    fields.target_id = "sub_ecu_denso_sh7055_02";
    fields.family_plan = SubaruDensoSh7055_02Plan{
        .tester_id = 0xf0,
        .target_id = 0x10,
        .read_ecu_id = true,
    };

    auto plan = validate_and_build(std::move(fields));

    ASSERT_TRUE(plan.has_value()) << plan.error().detail;
    EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoSh7055_02);
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
    fields.family_plan = MitsuColtM32rCanPlan{
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
        .use_vendor_challenge = false,
        .session_id = 0x81,
    };
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
    fields.kernel = fastecu::flash::KernelImage{.id = "colt", .load_address = 0x800000, .bytes = {}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, fastecu::ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, testing::HasSubstr("kernel bytes"));
}

TEST(FlashValidation, RejectsAPresentKernelWithNoId)
{
    auto fields = kernellessReadFields();
    fields.kernel = fastecu::flash::KernelImage{.id = "", .load_address = 0x800000, .bytes = {0x01, 0x02}};

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
