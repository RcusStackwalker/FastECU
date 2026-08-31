#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"

#include <array>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"

namespace
{

using fastecu::ErrorKind;
using fastecu::flash::build_mitsu_colt_m32r_can_plan;
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

bytes::Bytes rom(std::uint32_t size)
{
    return bytes::Bytes(size, 0x00);
}

TEST(MitsuColtM32rCanPlan, RejectsProtocolNamesThatDoNotMatchExactly)
{
    // Prefix matching would let an unconfigured protocol select a flash
    // capacity, so the complete protocol identifier is the contract.
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, "mitsu_ecu_m32r_can_vendor_ext_512kb_typo",
                                                     kMcu512, std::nullopt);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(MitsuColtM32rCanPlan, ReadPlansSnapshotProtocolCapacityAndVendorChallenge)
{
    // A nonzero legacy block start or an incorrect length would cause a ROM
    // read to omit bytes or cross the selected capacity boundary.
    for (const VariantCase& test : kVariants)
    {
        const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, test.id, test.mcu, std::nullopt);

        ASSERT_TRUE(plan.has_value()) << test.id << ": " << plan.error().detail;
        EXPECT_EQ(plan->transfer_region().start, 0U) << test.id;
        EXPECT_EQ(plan->transfer_region().length, test.size) << test.id;
        const auto& family = std::get<MitsuColtM32rCanPlan>(plan->family_plan());
        EXPECT_EQ(family.use_vendor_challenge, test.vendor) << test.id;
        EXPECT_EQ(family.session_id, MitsuColtCan::kSessionBootload) << test.id;
        EXPECT_EQ(family.request_id, 0x7e0U) << test.id;
        EXPECT_EQ(family.response_id, 0x7e8U) << test.id;
        EXPECT_EQ(family.bitrate, 500000) << test.id;
        EXPECT_FALSE(family.extended_id) << test.id;
        EXPECT_TRUE(plan->erase_regions().empty()) << test.id;
        EXPECT_FALSE(plan->kernel().has_value()) << test.id;
        EXPECT_TRUE(plan->confirmations().empty()) << test.id;
    }
}

TEST(MitsuColtM32rCanPlan, WritePlansUseTheCapacitySpecificRangeAndConfirmations)
{
    // A capacity-independent write window or confirmation set could write
    // protected bytes or skip the 512 KiB top-region bootstrap gate.
    for (const VariantCase& test : kVariants)
    {
        const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, test.id, test.mcu, rom(test.size));

        ASSERT_TRUE(plan.has_value()) << test.id << ": " << plan.error().detail;
        EXPECT_EQ(plan->transfer_region().start, 0x8000U) << test.id;
        EXPECT_EQ(plan->transfer_region().length, test.size - 0x8000U) << test.id;
        ASSERT_TRUE(plan->image().has_value()) << test.id;
        EXPECT_EQ(plan->image()->size(), test.size) << test.id;
        if (test.size == 0x80000)
        {
            EXPECT_THAT(plan->confirmations(),
                        testing::ElementsAre(Field(&ConfirmationSpec::id, ConfirmationSpec::Id::EraseTrigger),
                                             Field(&ConfirmationSpec::id, ConfirmationSpec::Id::TopRegionBootstrap)))
                << test.id;
        }
        else
        {
            EXPECT_THAT(plan->confirmations(),
                        testing::ElementsAre(Field(&ConfirmationSpec::id, ConfirmationSpec::Id::EraseTrigger)))
                << test.id;
        }
    }
}

TEST(MitsuColtM32rCanPlan, RejectsImagesWhoseCapacityDoesNotMatchTheProtocol)
{
    // Accepting an image for the other capacity would make protocol selection
    // ineffective and can direct the ECU to erase/write the wrong extent.
    const auto plan384 = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kDefaultProtocol, kMcu384, rom(0x80000));
    ASSERT_FALSE(plan384.has_value());
    EXPECT_EQ(plan384.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan384.error().detail, HasSubstr("0x60000"));

    const auto plan512 =
        build_mitsu_colt_m32r_can_plan(FlashOperation::Write, "mitsu_ecu_m32r_can_512kb", kMcu512, rom(0x60000));
    ASSERT_FALSE(plan512.has_value());
    EXPECT_EQ(plan512.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan512.error().detail, HasSubstr("0x80000"));
}

TEST(MitsuColtM32rCanPlan, RejectsAnUnknownMcuType)
{
    const auto plan =
        build_mitsu_colt_m32r_can_plan(FlashOperation::Read, kDefaultProtocol, "NOT_A_REAL_MCU", std::nullopt);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, HasSubstr("Unknown MCU type: NOT_A_REAL_MCU"));
}

TEST(MitsuColtM32rCanPlan, RejectsProtocolAndMcuCapacityDisagreement)
{
    for (const auto [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"mitsu_ecu_m32r_can", kMcu512},
             {"mitsu_ecu_m32r_can_512kb", kMcu384},
         }))
    {
        const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Read, protocol, mcu, std::nullopt);

        ASSERT_FALSE(plan.has_value()) << protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig) << protocol;
    }
}

TEST(MitsuColtM32rCanPlan, RejectsTestWriteAsUnsupported)
{
    const auto plan =
        build_mitsu_colt_m32r_can_plan(FlashOperation::TestWrite, kDefaultProtocol, kMcu384, rom(0x60000));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::Unsupported);
    EXPECT_THAT(plan.error().detail, HasSubstr("test_write"));
}

TEST(MitsuColtM32rCanPlan, RejectsAnUnknownProtocolBeforeTestWriteCapabilityChecking)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::TestWrite, "mitsu_ecu_m32r_can_512kb_typo",
                                                     kMcu512, rom(0x80000));

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    EXPECT_THAT(plan.error().detail, HasSubstr("Unsupported Mitsubishi Colt M32R CAN protocol"));
}

TEST(MitsuColtM32rCanPlan, RejectsAWriteWithNoImage)
{
    const auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, kDefaultProtocol, kMcu384, std::nullopt);

    ASSERT_FALSE(plan.has_value());
    EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
}

TEST(MitsuColtM32rCanPlan, WriteConfirmationsCarryStableGeometryArguments)
{
    auto plan = build_mitsu_colt_m32r_can_plan(FlashOperation::Write, "mitsu_ecu_m32r_can_512kb", "M32R_512KB_1block",
                                               bytes::Bytes(0x80000));

    ASSERT_TRUE(plan.has_value());
    ASSERT_EQ(plan->confirmations().size(), 2U);
    EXPECT_EQ(plan->confirmations()[0].arguments,
              (std::vector<std::pair<std::string, std::string>>{
                  {"capacity_kib", "512"}, {"writable_start_hex", "0x8000"}, {"rom_end_hex", "0x80000"}}));
    EXPECT_EQ(plan->confirmations()[1].arguments,
              (std::vector<std::pair<std::string, std::string>>{{"top_region_start_hex", "0x60000"},
                                                                {"rom_end_hex", "0x80000"}}));
}

} // namespace
