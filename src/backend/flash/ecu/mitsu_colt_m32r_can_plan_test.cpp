#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <array>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::flash::BuildMitsuColtM32rCanPlan;
using fastecu::flash::ConfirmationSpec;
using fastecu::flash::FlashOperation;
using fastecu::flash::MitsuColtM32rCanPlan;
using testing::Field;
using testing::HasSubstr;

constexpr std::string_view kDefaultProtocol = "mitsu_ecu_m32r_can";
constexpr std::string_view kMcu384 = "M32R_384KB_1block";
constexpr std::string_view kMcu512 = "M32R_512KB_1block";

struct VariantCase
{
    std::string_view id;
    std::string_view mcu;
    bool vendor;
    std::uint32_t size;
};

constexpr auto kVariants = std::to_array<VariantCase>({
    {"mitsu_ecu_m32r_can", kMcu384, false, 0x60000},
    {"mitsu_ecu_m32r_can_vendor_ext", kMcu384, true, 0x60000},
    {"mitsu_ecu_m32r_can_512kb", kMcu512, false, 0x80000},
    {"mitsu_ecu_m32r_can_vendor_ext_512kb", kMcu512, true, 0x80000},
});

bytes::Bytes Rom(std::uint32_t size)
{
    return bytes::Bytes(size, 0x00);
}

TEST(MitsuColtM32rCanPlan, RejectsProtocolNamesThatDoNotMatchExactly)
{
    // Prefix matching would let an unconfigured protocol select a flash
    // capacity, so the complete protocol identifier is the contract.
    ASSERT_THAT(BuildMitsuColtM32rCanPlan(FlashOperation::kRead, "mitsu_ecu_m32r_can_vendor_ext_512kb_typo", kMcu512,
                                          std::nullopt),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(MitsuColtM32rCanPlan, ReadPlansSnapshotProtocolCapacityAndVendorChallenge)
{
    // A nonzero legacy block start or an incorrect length would cause a ROM
    // read to omit bytes or cross the selected capacity boundary.
    for (const VariantCase& test : kVariants)
    {
        const auto plan = BuildMitsuColtM32rCanPlan(FlashOperation::kRead, test.id, test.mcu, std::nullopt);

        ASSERT_THAT(plan, fastecu::testing::IsOk()) << test.id << ": " << plan.error().detail;
        EXPECT_EQ(plan->TransferRegion().start, 0U) << test.id;
        EXPECT_EQ(plan->TransferRegion().length, test.size) << test.id;
        const auto& family = std::get<MitsuColtM32rCanPlan>(plan->FamilyPlan());
        EXPECT_EQ(family.use_vendor_challenge, test.vendor) << test.id;
        EXPECT_EQ(family.session_id, mitsu_colt_can::kSessionBootload) << test.id;
        EXPECT_EQ(family.request_id, 0x7e0U) << test.id;
        EXPECT_EQ(family.response_id, 0x7e8U) << test.id;
        EXPECT_EQ(family.bitrate, 500000) << test.id;
        EXPECT_FALSE(family.extended_id) << test.id;
        EXPECT_TRUE(plan->EraseRegions().empty()) << test.id;
        EXPECT_FALSE(plan->Kernel().has_value()) << test.id;
        EXPECT_TRUE(plan->Confirmations().empty()) << test.id;
    }
}

TEST(MitsuColtM32rCanPlan, WritePlansUseTheCapacitySpecificRangeAndConfirmations)
{
    // A capacity-independent write window or confirmation set could write
    // protected bytes or skip the 512 KiB top-region bootstrap gate.
    for (const VariantCase& test : kVariants)
    {
        const auto plan = BuildMitsuColtM32rCanPlan(FlashOperation::kWrite, test.id, test.mcu, Rom(test.size));

        ASSERT_THAT(plan, fastecu::testing::IsOk()) << test.id << ": " << plan.error().detail;
        EXPECT_EQ(plan->TransferRegion().start, 0x8000U) << test.id;
        EXPECT_EQ(plan->TransferRegion().length, test.size - 0x8000U) << test.id;
        ASSERT_TRUE(plan->Image().has_value()) << test.id;
        EXPECT_EQ(plan->Image()->size(), test.size) << test.id;
        if (test.size == 0x80000)
        {
            EXPECT_THAT(plan->Confirmations(),
                        testing::ElementsAre(Field(&ConfirmationSpec::id, ConfirmationSpec::Id::kEraseTrigger),
                                             Field(&ConfirmationSpec::id, ConfirmationSpec::Id::kTopRegionBootstrap)))
                << test.id;
        }
        else
        {
            EXPECT_THAT(plan->Confirmations(),
                        testing::ElementsAre(Field(&ConfirmationSpec::id, ConfirmationSpec::Id::kEraseTrigger)))
                << test.id;
        }
    }
}

TEST(MitsuColtM32rCanPlan, RejectsImagesWhoseCapacityDoesNotMatchTheProtocol)
{
    // Accepting an image for the other capacity would make protocol selection
    // ineffective and can direct the ECU to erase/write the wrong extent.
    const auto plan384 = BuildMitsuColtM32rCanPlan(FlashOperation::kWrite, kDefaultProtocol, kMcu384, Rom(0x80000));
    ASSERT_THAT(plan384, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan384.error().detail, HasSubstr("0x60000"));

    const auto plan512 =
        BuildMitsuColtM32rCanPlan(FlashOperation::kWrite, "mitsu_ecu_m32r_can_512kb", kMcu512, Rom(0x60000));
    ASSERT_THAT(plan512, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan512.error().detail, HasSubstr("0x80000"));
}

TEST(MitsuColtM32rCanPlan, RejectsAnUnknownMcuType)
{
    const auto plan =
        BuildMitsuColtM32rCanPlan(FlashOperation::kRead, kDefaultProtocol, "NOT_A_REAL_MCU", std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, HasSubstr("Unknown MCU type: NOT_A_REAL_MCU"));
}

TEST(MitsuColtM32rCanPlan, RejectsProtocolAndMcuCapacityDisagreement)
{
    for (const auto [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"mitsu_ecu_m32r_can", kMcu512},
             {"mitsu_ecu_m32r_can_512kb", kMcu384},
         }))
    {
        ASSERT_THAT(BuildMitsuColtM32rCanPlan(FlashOperation::kRead, protocol, mcu, std::nullopt),
                    fastecu::testing::IsErr(ErrorKind::kInvalidConfig))
            << protocol;
    }
}

TEST(MitsuColtM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan = BuildMitsuColtM32rCanPlan(FlashOperation::kTestWrite, kDefaultProtocol, kMcu384, Rom(0x60000));

    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kUnsupported));
    EXPECT_THAT(plan.error().detail, HasSubstr("test_write"));
}

TEST(MitsuColtM32rCanPlan, RejectsAnUnknownProtocolBeforeTestWriteCapabilityChecking)
{
    const auto plan =
        BuildMitsuColtM32rCanPlan(FlashOperation::kTestWrite, "mitsu_ecu_m32r_can_512kb_typo", kMcu512, Rom(0x80000));

    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, HasSubstr("Unsupported Mitsubishi Colt M32R CAN protocol"));
}

TEST(MitsuColtM32rCanPlan, RejectsAWriteWithNoImage)
{
    ASSERT_THAT(BuildMitsuColtM32rCanPlan(FlashOperation::kWrite, kDefaultProtocol, kMcu384, std::nullopt),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(MitsuColtM32rCanPlan, WriteConfirmationsCarryStableGeometryArguments)
{
    auto plan = BuildMitsuColtM32rCanPlan(FlashOperation::kWrite, "mitsu_ecu_m32r_can_512kb", "M32R_512KB_1block",
                                          bytes::Bytes(0x80000));

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ASSERT_EQ(plan->Confirmations().size(), 2U);
    EXPECT_EQ(plan->Confirmations()[0].arguments,
              (std::vector<std::pair<std::string, std::string>>{
                  {"capacity_kib", "512"}, {"writable_start_hex", "0x8000"}, {"rom_end_hex", "0x80000"}}));
    EXPECT_EQ(plan->Confirmations()[1].arguments,
              (std::vector<std::pair<std::string, std::string>>{{"top_region_start_hex", "0x60000"},
                                                                {"rom_end_hex", "0x80000"}}));
}

} // namespace
