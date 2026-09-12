#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"

#include <array>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

// Independently transcribed from kernelmemorymodels.h:124-141 and 209-214;
// these literals deliberately do not use the production flash-device lookup.
constexpr std::array<MemoryRegion, 16> kSh7058Blocks{{
    {0x00000000, 0x00001000},
    {0x00001000, 0x00001000},
    {0x00002000, 0x00001000},
    {0x00003000, 0x00001000},
    {0x00004000, 0x00001000},
    {0x00005000, 0x00001000},
    {0x00006000, 0x00001000},
    {0x00007000, 0x00001000},
    {0x00008000, 0x00018000},
    {0x00020000, 0x00020000},
    {0x00040000, 0x00020000},
    {0x00060000, 0x00020000},
    {0x00080000, 0x00020000},
    {0x000A0000, 0x00020000},
    {0x000C0000, 0x00020000},
    {0x000E0000, 0x00020000},
}};

struct Variant
{
    std::string_view protocol;
    SubaruDensoSh7058CanSecurity security;
};

constexpr std::array<Variant, 5> kVariants{{
    {"sub_ecu_denso_sh7058_can", SubaruDensoSh7058CanSecurity::Stock},
    {"sub_ecu_denso_sh7058_can_ecutek", SubaruDensoSh7058CanSecurity::EcuTek},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", SubaruDensoSh7058CanSecurity::RaceRom},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", SubaruDensoSh7058CanSecurity::RaceRomAlt},
    {"sub_ecu_denso_sh7058_can_cobb", SubaruDensoSh7058CanSecurity::Cobb},
}};

constexpr std::uint32_t kRomSize = 0x00100000;
constexpr std::uint32_t kKernelAddress = 0xFFFF3000;
constexpr std::uint32_t kKernelRegionSize = 0x00009000;

KernelImage kernel(bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = "petrol-sh7058-kernel", .load_address = kKernelAddress, .bytes = std::move(data)};
}

std::optional<bytes::Bytes> image_for(FlashOperation operation)
{
    if (operation == FlashOperation::Read)
    {
        return std::nullopt;
    }
    return bytes::Bytes(kRomSize, bytes::Byte{0});
}

FlashPlanFields valid_fields(const Variant& variant, FlashOperation operation = FlashOperation::Write)
{
    return {
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh7058Can,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(variant.protocol),
        .mcu_name = "SH7058",
        .transfer_region = {0x00000000, kRomSize},
        .erase_regions = operation == FlashOperation::Read
                             ? std::vector<MemoryRegion>{}
                             : std::vector<MemoryRegion>(kSh7058Blocks.begin(), kSh7058Blocks.end()),
        .image = image_for(operation),
        .kernel = kernel(),
        .family_plan = SubaruDensoSh7058CanPlan{.security = variant.security},
        .confirmations = {},
    };
}

TEST(SubaruDensoSh7058CanPlan, BuildsEveryExactSecurityVariantOperationAndGeometry)
{
    for (const Variant& variant : kVariants)
    {
        for (const FlashOperation operation : {FlashOperation::Read, FlashOperation::TestWrite, FlashOperation::Write})
        {
            auto plan = build_subaru_denso_sh7058_can_plan(operation, variant.protocol, "SH7058", image_for(operation),
                                                           kernel());

            ASSERT_TRUE(plan.has_value()) << variant.protocol << ": " << plan.error().detail;
            EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoSh7058Can);
            EXPECT_EQ(plan->transport(), TransportKind::CanIso15765);
            EXPECT_EQ(plan->target_id(), variant.protocol);
            EXPECT_EQ(plan->mcu_name(), "SH7058");
            EXPECT_EQ(plan->transfer_region().start, 0U);
            EXPECT_EQ(plan->transfer_region().length, kRomSize);
            EXPECT_TRUE(plan->confirmations().empty());
            ASSERT_TRUE(plan->kernel().has_value());
            EXPECT_EQ(plan->kernel()->id, "petrol-sh7058-kernel");
            EXPECT_EQ(plan->kernel()->load_address, kKernelAddress);

            const auto& wire = std::get<SubaruDensoSh7058CanPlan>(plan->family_plan());
            EXPECT_EQ(wire.request_id, 0x7E0U);
            EXPECT_EQ(wire.response_id, 0x7E8U);
            EXPECT_EQ(wire.bitrate, 500000);
            EXPECT_FALSE(wire.extended_id);
            EXPECT_EQ(wire.security, variant.security);

            if (operation == FlashOperation::Read)
            {
                EXPECT_FALSE(plan->image().has_value());
                EXPECT_TRUE(plan->erase_regions().empty());
            }
            else
            {
                ASSERT_TRUE(plan->image().has_value());
                EXPECT_EQ(plan->image()->size(), kRomSize);
                ASSERT_EQ(plan->erase_regions().size(), kSh7058Blocks.size());
                for (std::size_t block = 0; block < kSh7058Blocks.size(); ++block)
                {
                    EXPECT_EQ(plan->erase_regions()[block].start, kSh7058Blocks[block].start);
                    EXPECT_EQ(plan->erase_regions()[block].length, kSh7058Blocks[block].length);
                }
            }
            EXPECT_TRUE(validate_subaru_denso_sh7058_can_plan(*plan).has_value());
        }
    }
}

TEST(SubaruDensoSh7058CanPlan, RejectsUnknownSuffixDieselDensoCanTcuAndWrongMcuIdentities)
{
    constexpr std::array<std::string_view, 8> kRejected{{
        "unknown_petrol_can",
        "sub_ecu_denso_sh7058_can_future",
        "sub_ecu_denso_sh7058_can_ecutek_extra",
        "sub_ecu_denso_sh7058_can_diesel",
        "sub_ecu_denso_sh7058_densocan",
        "sub_ecu_denso_sh7058s_diesel_densocan",
        "sub_tcu_denso_sh7058_can",
        "sub_tcu_denso_sh7055_can",
    }};
    for (const std::string_view protocol : kRejected)
    {
        auto plan =
            build_subaru_denso_sh7058_can_plan(FlashOperation::Read, protocol, "SH7058", std::nullopt, kernel());
        ASSERT_FALSE(plan.has_value()) << protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }

    for (const Variant& variant : kVariants)
    {
        auto plan = build_subaru_denso_sh7058_can_plan(FlashOperation::Read, variant.protocol, "SH7058d", std::nullopt,
                                                       kernel());
        ASSERT_FALSE(plan.has_value()) << variant.protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoSh7058CanPlan, EnforcesReadAndWriteImageRequirementsExactly)
{
    const Variant& stock = kVariants.front();
    auto read_with_image = build_subaru_denso_sh7058_can_plan(FlashOperation::Read, stock.protocol, "SH7058",
                                                              bytes::Bytes(kRomSize, 0), kernel());
    ASSERT_FALSE(read_with_image.has_value());
    EXPECT_EQ(read_with_image.error().kind, ErrorKind::InvalidConfig);

    for (const FlashOperation operation : {FlashOperation::TestWrite, FlashOperation::Write})
    {
        const std::array<std::optional<bytes::Bytes>, 3> invalid{{
            std::nullopt,
            std::optional<bytes::Bytes>{bytes::Bytes(kRomSize - 1, 0)},
            std::optional<bytes::Bytes>{bytes::Bytes(kRomSize + 1, 0)},
        }};
        for (const auto& image : invalid)
        {
            auto plan = build_subaru_denso_sh7058_can_plan(operation, stock.protocol, "SH7058", image, kernel());
            ASSERT_FALSE(plan.has_value());
            EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
        }
    }
}

TEST(SubaruDensoSh7058CanPlan, EnforcesKernelIdentityAddressAnd128BytePaddedContainingRegion)
{
    const Variant& stock = kVariants.front();
    auto exact = build_subaru_denso_sh7058_can_plan(FlashOperation::Read, stock.protocol, "SH7058", std::nullopt,
                                                    kernel(bytes::Bytes(kKernelRegionSize, bytes::Byte{0xA5})));
    ASSERT_TRUE(exact.has_value()) << exact.error().detail;

    for (KernelImage invalid : {
             KernelImage{.id = "", .load_address = kKernelAddress, .bytes = {0x01}},
             KernelImage{.id = "wrong-address", .load_address = kKernelAddress + 1, .bytes = {0x01}},
             KernelImage{.id = "empty", .load_address = kKernelAddress, .bytes = {}},
             kernel(bytes::Bytes(kKernelRegionSize + 1, bytes::Byte{0xA5})),
         })
    {
        auto plan = build_subaru_denso_sh7058_can_plan(FlashOperation::Read, stock.protocol, "SH7058", std::nullopt,
                                                       std::move(invalid));
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoSh7058CanPlan, ValidatorRejectsWireSecurityRegionGeometryAndConfirmationDrift)
{
    for (int mutation = 0; mutation < 9; ++mutation)
    {
        auto fields = valid_fields(kVariants.front());
        auto& wire = std::get<SubaruDensoSh7058CanPlan>(fields.family_plan);
        switch (mutation)
        {
        case 0:
            wire.request_id = 0x7E1;
            break;
        case 1:
            wire.response_id = 0x7E9;
            break;
        case 2:
            wire.bitrate = 250000;
            break;
        case 3:
            wire.extended_id = true;
            break;
        case 4:
            wire.security = SubaruDensoSh7058CanSecurity::EcuTek;
            break;
        case 5:
            fields.transfer_region.length -= 1;
            break;
        case 6:
            fields.erase_regions.pop_back();
            break;
        case 7:
            fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}};
            break;
        case 8:
            // Security is carried by the validated enum, never inferred from
            // a mutable target-id suffix.
            fields.target_id = "sub_ecu_denso_sh7058_can_ecutek";
            break;
        }
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto valid = validate_subaru_denso_sh7058_can_plan(*plan);
        ASSERT_FALSE(valid.has_value()) << mutation;
        EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
    }
}

} // namespace
} // namespace fastecu::flash
