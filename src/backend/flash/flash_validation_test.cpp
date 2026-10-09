#include "src/backend/ports/testing/result_matchers.h"
// src/backend/flash/flash_validation_test.cpp
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <tuple>
#include <type_traits>
#include <variant>

namespace fastecu::flash
{
namespace
{

FlashPlanFields valid_read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromKline,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0xf000, .length = 0x1000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff2000, .bytes = {0x01}},
        .family_plan =
            DensoSh705xEepromKlinePlan{
                .mode = EepromReadMode::kMode2,
                .security = DensoSecurityVariant::kStock,
                .tester_id = 0xf0,
                .target_id = 0x10,
                .initial_baud = 4800,
                .kernel_baud = 15625,
            },
        .confirmations =
            {
                ConfirmationSpec{.id = ConfirmationSpec::Id::kBeginEepromRead},
                ConfirmationSpec{.id = ConfirmationSpec::Id::kInspectEepromBytes},
            },
    };
}

FlashPlanFields valid_can_read_fields()
{
    auto fields = valid_read_fields();
    fields.family = FlashFamily::kDensoSh705xEepromCan;
    fields.transport = TransportKind::kCanIso15765;
    fields.kernel = KernelImage{.id = "k", .load_address = 0xffff3000, .bytes = {0x01}};
    fields.family_plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::kMode2,
        .security = DensoSecurityVariant::kStock,
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

const std::array<FamilyCase, 30>& family_cases()
{
    static const std::array<FamilyCase, 30> cases{{
        {FlashFamily::kDensoSh705xEepromKline, TransportKind::kKline,
         DensoSh705xEepromKlinePlan{.mode = EepromReadMode::kMode2,
                                    .security = DensoSecurityVariant::kStock,
                                    .tester_id = 0xf0,
                                    .target_id = 0x10,
                                    .initial_baud = 4800,
                                    .kernel_baud = 15625},
         "DensoSh705xEepromKline"},
        {FlashFamily::kDensoSh705xEepromCan, TransportKind::kCanIso15765,
         DensoSh705xEepromCanPlan{.mode = EepromReadMode::kMode2,
                                  .security = DensoSecurityVariant::kStock,
                                  .request_id = 0x7e0,
                                  .response_id = 0x7e8,
                                  .bitrate = 500000,
                                  .extended_id = false},
         "DensoSh705xEepromCan"},
        {FlashFamily::kMitsuColtM32rCan, TransportKind::kCanIso15765,
         MitsuColtM32rCanPlan{.request_id = 0x7e0,
                              .response_id = 0x7e8,
                              .bitrate = 500000,
                              .extended_id = false,
                              .use_vendor_challenge = false,
                              .session_id = 0x85},
         "MitsuColtM32rCan"},
        {FlashFamily::kSubaruMitsuM32rKline, TransportKind::kKline,
         SubaruMitsuM32rKlinePlan{.tester_id = 0xf0,
                                  .target_id = 0x10,
                                  .initial_baud = 4800,
                                  .flash_baud = 62500,
                                  .chunk_size = 0x200,
                                  .unread_prefix_fill = 0xff},
         "SubaruMitsuM32rKline"},
        {FlashFamily::kSubaruHitachiM32rKline, TransportKind::kKline,
         SubaruHitachiM32rKlinePlan{.session_mode = HitachiM32rKlineSessionMode::kNormal,
                                    .tester_id = 0xf0,
                                    .target_id = 0x10,
                                    .initial_baud = 4800,
                                    .write_baud = 62500,
                                    .read_baud = 62500,
                                    .chunk_size = 0x200,
                                    .read_address_bias = 0},
         "SubaruHitachiM32rKline"},
        {FlashFamily::kSubaruDensoMc68hc16y502, TransportKind::kKline,
         SubaruDensoMc68hc16y5_02Plan{.connect_baud = 9600,
                                      .kernel_baud = 9600,
                                      .encryption_xor = 0x55,
                                      .kernel_magic = 0x3941,
                                      .bootloader_ok = {0x4d, 0x00, 0xb3}},
         "SubaruDensoMc68hc16y5_02"},
        {FlashFamily::kSubaruDensoSh705502, TransportKind::kKline,
         SubaruDensoSh7055_02Plan{.tester_id = 0xf0, .target_id = 0x10, .read_ecu_id = true}, "SubaruDensoSh7055_02"},
        {FlashFamily::kSubaruHitachiM32rCan, TransportKind::kCanIso15765,
         SubaruHitachiM32rCanPlan{.request_id = 0x7e0, .response_id = 0x7e8, .bitrate = 500000, .extended_id = false},
         "SubaruHitachiM32rCan"},
        {FlashFamily::kSubaruTcuCvtHitachiM32rCan, TransportKind::kCanIso15765,
         SubaruTcuCvtHitachiM32rCanPlan{
             .request_id = 0x7e1, .response_id = 0x7e9, .bitrate = 500000, .extended_id = false},
         "SubaruTcuCvtHitachiM32rCan"},
        {FlashFamily::kSubaruTcuCvtMitsuMh8111Can, TransportKind::kCanIso15765,
         SubaruTcuCvtMitsuMh8111CanPlan{
             .request_id = 0x7e1, .response_id = 0x7e9, .bitrate = 500000, .extended_id = false},
         "SubaruTcuCvtMitsuMh8111Can"},
        {FlashFamily::kSubaruTcuCvtMitsuMh8104Can, TransportKind::kCanIso15765,
         SubaruTcuCvtMitsuMh8104CanPlan{
             .request_id = 0x7e1, .response_id = 0x7e9, .bitrate = 500000, .extended_id = false},
         "SubaruTcuCvtMitsuMh8104Can"},
        {FlashFamily::kSubaruDenso1n83m15mCan, TransportKind::kCanIso15765,
         SubaruDenso1n83m_1_5mCanPlan{.request_id = 0x7e0,
                                      .response_id = 0x7e8,
                                      .bitrate = 500000,
                                      .extended_id = false,
                                      .lead_pad_len = 0x10000,
                                      .tail_pad_len = 0x100},
         "SubaruDenso1n83m_1_5mCan"},
        {FlashFamily::kSubaruDensoSh72531Can, TransportKind::kCanIso15765,
         SubaruDensoSh72531CanPlan{.request_id = 0x7e0,
                                   .response_id = 0x7e8,
                                   .bitrate = 500000,
                                   .extended_id = false,
                                   .lead_pad_len = 0x8000,
                                   .tail_pad_len = 0x100},
         "SubaruDensoSh72531Can"},
        {FlashFamily::kSubaruDensoSh72543CanDiesel, TransportKind::kCanIso15765,
         SubaruDensoSh72543CanDieselPlan{.request_id = 0x7e0,
                                         .response_id = 0x7e8,
                                         .bitrate = 500000,
                                         .extended_id = false,
                                         .lead_pad_len = 0x8000,
                                         .tail_pad_len = 0x100},
         "SubaruDensoSh72543CanDiesel"},
        {FlashFamily::kSubaruDenso1n83m4mCan, TransportKind::kCanIso15765,
         SubaruDenso1n83m_4mCanPlan{.request_id = 0x7e0,
                                    .response_id = 0x7e8,
                                    .bitrate = 500000,
                                    .extended_id = false,
                                    .lead_pad_len = 0x10000,
                                    .tail_pad_len = 0x100},
         "SubaruDenso1n83m_4mCan"},
        {FlashFamily::kSubaruDensoSh705xDensoCan, TransportKind::kCanRawIso15765,
         SubaruDensoSh705xDensoCanPlan{.iso_request_id = 0x7e0,
                                       .iso_response_id = 0x7e8,
                                       .raw_transmit_id = 0x000ffffe,
                                       .raw_receive_id = 0x21,
                                       .bitrate = 500000,
                                       .iso_extended_id = false,
                                       .raw_extended_id = true},
         "SubaruDensoSh705xDensoCan"},
        {FlashFamily::kSubaruTcuDensoSh705xCan, TransportKind::kCanIso15765,
         SubaruTcuDensoSh705xCanPlan{
             .request_id = 0x7e1, .response_id = 0x7e9, .bitrate = 500000, .extended_id = false},
         "SubaruTcuDensoSh705xCan"},
        {FlashFamily::kSubaruDensoSh7058Can, TransportKind::kCanIso15765,
         SubaruDensoSh7058CanPlan{.request_id = 0x7e0,
                                  .response_id = 0x7e8,
                                  .bitrate = 500000,
                                  .extended_id = false,
                                  .security = SubaruDensoSh7058CanSecurity::kStock},
         "SubaruDensoSh7058Can"},
        {FlashFamily::kSubaruDensoSh7058CanDiesel, TransportKind::kCanIso15765,
         SubaruDensoSh7058CanDieselPlan{
             .request_id = 0x7e0, .response_id = 0x7e8, .bitrate = 500000, .extended_id = false},
         "SubaruDensoSh7058CanDiesel"},
        {FlashFamily::kSubaruTcuHitachiM32rKline, TransportKind::kKline,
         SubaruTcuHitachiM32rKlinePlan{.tester_id = 0xf0, .target_id = 0x18, .baud = 4800, .block_size = 96},
         "SubaruTcuHitachiM32rKline"},
        {FlashFamily::kSubaruTcuHitachiM32rCan, TransportKind::kCanIso15765,
         SubaruTcuHitachiM32rCanPlan{.request_id = 0x7e1,
                                     .response_id = 0x7e9,
                                     .bitrate = 500000,
                                     .extended_id = false,
                                     .page_size = 0x100,
                                     .write_frame_size = 128},
         "SubaruTcuHitachiM32rCan"},
        {FlashFamily::kSubaruHitachiSh72543rCan, TransportKind::kCanIso15765,
         SubaruHitachiSh72543rCanPlan{0x7e0, 0x7e8, 500000, false, 0x400, 0x100}, "SubaruHitachiSh72543rCan"},
        {FlashFamily::kSubaruHitachiSh7058, TransportKind::kKline, SubaruHitachiSh7058KlinePlan{},
         "SubaruHitachiSh7058"},
        {FlashFamily::kSubaruHitachiSh7058, TransportKind::kCanIso15765, SubaruHitachiSh7058CanPlan{},
         "SubaruHitachiSh7058"},
        {FlashFamily::kSubaruUnisiaJecs, TransportKind::kKline,
         SubaruUnisiaJecsPlan{.initial_baud = 1953, .even_parity = true}, "SubaruUnisiaJecs"},
        {FlashFamily::kSubaruDensoSh705xKline, TransportKind::kKline,
         SubaruDensoSh705xKlinePlan{.initial_baud = 4800,
                                    .tester_id = 0xF0,
                                    .target_id = 0x10,
                                    .seed_key = SubaruDensoSh705xKlineSeedKey::kStock},
         "SubaruDensoSh705xKline"},
        {FlashFamily::kSubaruDensoMc68hc16y502Bdm, TransportKind::kKline,
         SubaruDensoMc68hc16y5_02BdmPlan{.baud = 115200}, "SubaruDensoMc68hc16y5_02Bdm"},
        {FlashFamily::kSubaruUnisiaJecsM32rKline, TransportKind::kKline,
         SubaruUnisiaJecsM32rKlinePlan{.initial_baud = 4800, .tester_id = 0xf0, .target_id = 0x10},
         "SubaruUnisiaJecsM32rKline"},
        {FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel, TransportKind::kKline,
         SubaruUnisiaJecsM32rBootModeKernelPlan{.initial_baud = 39063, .tester_id = 0xf0, .target_id = 0x10},
         "SubaruUnisiaJecsM32rBootModeKernel"},
        {FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram, TransportKind::kKline,
         SubaruUnisiaJecsM32rBootModeProgramPlan{.initial_baud = 19200, .tester_id = 0xf0, .target_id = 0x10},
         "SubaruUnisiaJecsM32rBootModeProgram"},
    }};
    return cases;
}

static_assert(std::variant_size_v<FamilyPlan> == std::tuple_size_v<std::remove_reference_t<decltype(family_cases())>>);

TEST(FlashValidationTest, ValidReadFieldsProduceAPlan)
{
    auto plan = validate_and_build(valid_read_fields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->total_transfer_bytes(), 0x1000U);
}

TEST(FlashValidationTest, ReadPlanHasNoImageButKeepsItsKernel)
{
    auto plan = validate_and_build(valid_read_fields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    EXPECT_TRUE(plan->image_or_empty().empty());
    EXPECT_EQ(plan->kernel_or_empty().id, "k");
    EXPECT_EQ(plan->kernel_or_empty().load_address, 0xffff2000U);
    EXPECT_EQ(plan->kernel_or_empty().bytes, bytes::Bytes{0x01});
}

TEST(FlashValidationTest, WritePlanExposesItsImage)
{
    auto fields = valid_read_fields();
    fields.operation = FlashOperation::kWrite;
    fields.image = bytes::Bytes(0x1000, 0xA5);
    auto plan = validate_and_build(std::move(fields));
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    EXPECT_EQ(plan->image_or_empty().size(), 0x1000U);
    EXPECT_EQ(plan->image(), bytes::Bytes(0x1000, 0xA5));
}

TEST(FlashValidationTest, EmptyTargetIdIsRejected)
{
    auto fields = valid_read_fields();
    fields.target_id.clear();

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(FlashValidationTest, EmptyMcuNameIsRejected)
{
    auto fields = valid_read_fields();
    fields.mcu_name.clear();

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(FlashValidationTest, ZeroLengthTransferRegionIsRejected)
{
    auto fields = valid_read_fields();
    fields.transfer_region.length = 0;

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(FlashValidationTest, TransferRegionOverflowIsRejected)
{
    auto fields = valid_read_fields();
    fields.transfer_region = MemoryRegion{.start = 0xffffffff, .length = 0x10};

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

// A region whose last byte is 0xffffffff ends at 0x100000000, which is one
// past what a uint32_t end address can hold. Executors compute that end
// address in uint32_t arithmetic and bound real write loops with it -- see
// ecu/mitsu_colt_m32r_can_executor.cpp's writable_end/page_write_end -- where
// it would wrap to 0 and collapse the write window. Rejecting it here is the
// invariant those call sites rely on, not an off-by-one: do not "fix" this
// into an acceptance.
TEST(FlashValidationTest, TransferRegionEndingExactlyAtTheTopOfTheAddressSpaceIsRejected)
{
    auto fields = valid_read_fields();
    fields.transfer_region = MemoryRegion{.start = 0xffffff00, .length = 0x100};

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

// The largest region this accepts, one byte short of the boundary above.
TEST(FlashValidationTest, TransferRegionEndingOneByteBelowTheTopOfTheAddressSpaceIsAccepted)
{
    auto fields = valid_read_fields();
    fields.transfer_region = MemoryRegion{.start = 0xffffff00, .length = 0xff};

    EXPECT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsOk());
}

TEST(FlashValidationTest, ReadWithNonEmptyEraseRegionsIsRejected)
{
    auto fields = valid_read_fields();
    fields.erase_regions.push_back(MemoryRegion{.start = 0, .length = 4});

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(FlashValidationTest, ReadWithImagePresentIsRejected)
{
    auto fields = valid_read_fields();
    fields.image = bytes::Bytes{0x00};

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(FlashValidationTest, EmptyKernelIdIsRejected)
{
    auto fields = valid_read_fields();
    ASSERT_TRUE(fields.kernel.has_value());
    fields.kernel->id.clear();

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(FlashValidationTest, EmptyKernelBytesIsRejected)
{
    auto fields = valid_read_fields();
    ASSERT_TRUE(fields.kernel.has_value());
    fields.kernel->bytes.clear();

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

// A family that isn't the kernel-less Mitsu Colt CAN family must still carry
// a kernel -- the optional relaxation is scoped to
// MitsuColtM32rCan (kFamilyRequiresKernel's specialization), not a
// blanket relaxation for every family.
TEST(FlashValidationTest, MissingKernelIsRejectedForKlineFamilyByDefault)
{
    auto fields = valid_read_fields();
    fields.kernel = std::nullopt;

    auto plan = validate_and_build(std::move(fields));

    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, ::testing::HasSubstr("kernel"));
}

TEST(FlashValidationTest, MissingKernelIsRejectedForCanFamilyByDefault)
{
    auto fields = valid_can_read_fields();
    fields.kernel = std::nullopt;

    auto plan = validate_and_build(std::move(fields));

    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, ::testing::HasSubstr("kernel"));
}

TEST(FlashValidationTest, FamilyPlanTagMismatchWithTransportIsRejected)
{
    auto fields = valid_read_fields();
    // Kline transport but a Can family_plan variant.
    fields.family_plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::kMode2,
        .security = DensoSecurityVariant::kStock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
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
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        EXPECT_EQ(plan->experimental_family_id(), family_case.id);
    }
}

TEST(FlashValidationTest, Sh7055_02KlinePlanIsAccepted)
{
    auto fields = valid_read_fields();
    fields.family = FlashFamily::kSubaruDensoSh705502;
    fields.target_id = "sub_ecu_denso_sh7055_02";
    fields.family_plan = SubaruDensoSh7055_02Plan{
        .tester_id = 0xf0,
        .target_id = 0x10,
        .read_ecu_id = true,
    };

    auto plan = validate_and_build(std::move(fields));

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->family(), FlashFamily::kSubaruDensoSh705502);
}

TEST(FlashValidationTest, DuplicateConfirmationIdsAreRejected)
{
    auto fields = valid_read_fields();
    fields.confirmations.push_back(ConfirmationSpec{.id = ConfirmationSpec::Id::kBeginEepromRead});

    ASSERT_THAT(validate_and_build(std::move(fields)), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

// The "at least one confirmation" floor was removed:
// families whose read path prompts for nothing, like Mitsu Colt CAN, must be
// able to build a plan with zero confirmations. This is no longer rejected.
TEST(FlashValidationTest, ZeroConfirmationsIsNowAccepted)
{
    auto fields = valid_read_fields();
    fields.confirmations.clear();

    auto plan = validate_and_build(std::move(fields));

    ASSERT_THAT(plan, fastecu::testing::IsOk());
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
    fields.operation = FlashOperation::kRead;
    fields.family = FlashFamily::kMitsuColtM32rCan;
    fields.transport = TransportKind::kCanIso15765;
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

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_TRUE(plan->confirmations().empty());
    EXPECT_EQ(plan->experimental_family_id(), "MitsuColtM32rCan");
}

TEST(FlashValidation, RejectsAPresentKernelWithNoBytes)
{
    auto fields = kernellessReadFields();
    fields.kernel = fastecu::flash::KernelImage{.id = "colt", .load_address = 0x800000, .bytes = {}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_THAT(plan, fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, ::testing::HasSubstr("kernel bytes"));
}

TEST(FlashValidation, RejectsAPresentKernelWithNoId)
{
    auto fields = kernellessReadFields();
    fields.kernel = fastecu::flash::KernelImage{.id = "", .load_address = 0x800000, .bytes = {0x01, 0x02}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_THAT(plan, fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, ::testing::HasSubstr("kernel id"));
}

TEST(FlashValidation, RejectsAColtPlanOnAKlineTransport)
{
    auto fields = kernellessReadFields();
    fields.transport = fastecu::flash::TransportKind::kKline;

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_THAT(plan, fastecu::testing::IsErr(fastecu::ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, ::testing::HasSubstr("does not match transport kind"));
}

TEST(FlashValidation, StillRejectsDuplicateConfirmationIds)
{
    using fastecu::flash::ConfirmationSpec;
    auto fields = kernellessReadFields();
    fields.operation = fastecu::flash::FlashOperation::kWrite;
    fields.image = bytes::Bytes(0x80000, 0x00);
    fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::kEraseTrigger, {}},
                            ConfirmationSpec{ConfirmationSpec::Id::kEraseTrigger, {}}};

    const auto plan = fastecu::flash::validate_and_build(std::move(fields));

    ASSERT_THAT(plan, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(plan.error().detail, ::testing::HasSubstr("duplicate confirmation id"));
}

TEST(FlashValidation, KernellessPlanYieldsAnEmptyKernel)
{
    auto plan = fastecu::flash::validate_and_build(kernellessReadFields());
    ASSERT_THAT(plan, fastecu::testing::IsOk());

    EXPECT_TRUE(plan->kernel_or_empty().id.empty());
    EXPECT_TRUE(plan->kernel_or_empty().bytes.empty());
}
