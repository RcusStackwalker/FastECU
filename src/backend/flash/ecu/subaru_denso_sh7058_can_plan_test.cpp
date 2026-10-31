#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_types.h"

#include <array>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/ports/testing/result_matchers.h"

using ::testing::_;
using ::testing::Eq;
using ::testing::IsEmpty;
using ::testing::Optional;

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
    {"sub_ecu_denso_sh7058_can", SubaruDensoSh7058CanSecurity::kStock},
    {"sub_ecu_denso_sh7058_can_ecutek", SubaruDensoSh7058CanSecurity::kEcuTek},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom", SubaruDensoSh7058CanSecurity::kRaceRom},
    {"sub_ecu_denso_sh7058_can_ecutek_racerom_alt", SubaruDensoSh7058CanSecurity::kRaceRomAlt},
    {"sub_ecu_denso_sh7058_can_cobb", SubaruDensoSh7058CanSecurity::kCobb},
}};

constexpr std::uint32_t kRomSize = 0x00100000;
constexpr std::uint32_t kKernelAddress = 0xFFFF3000;
constexpr std::uint32_t kKernelRegionSize = 0x00009000;

KernelImage Kernel(bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = "petrol-sh7058-kernel", .load_address = kKernelAddress, .bytes = std::move(data)};
}

std::optional<bytes::Bytes> ImageFor(FlashOperation operation)
{
    if (operation == FlashOperation::kRead)
    {
        return std::nullopt;
    }
    return bytes::Bytes(kRomSize, bytes::Byte{0});
}

FlashPlanFields ValidFields(const Variant& variant, FlashOperation operation = FlashOperation::kWrite)
{
    return {
        .operation = operation,
        .target_id = std::string(variant.protocol),
        .mcu_name = "SH7058",
        .transfer_region = {0x00000000, kRomSize},
        .erase_regions = operation == FlashOperation::kRead
                             ? std::vector<MemoryRegion>{}
                             : std::vector<MemoryRegion>(kSh7058Blocks.begin(), kSh7058Blocks.end()),
        .image = ImageFor(operation),
        .kernel = Kernel(),
        .family_plan = SubaruDensoSh7058CanPlan{.security = variant.security},
        .confirmations = {},
    };
}

TEST(SubaruDensoSh7058CanPlan, BuildsEveryExactSecurityVariantOperationAndGeometry)
{
    for (const Variant& variant : kVariants)
    {
        SCOPED_TRACE(::testing::PrintToString(variant));
        for (const FlashOperation operation :
             {FlashOperation::kRead, FlashOperation::kTestWrite, FlashOperation::kWrite})
        {
            SCOPED_TRACE(::testing::PrintToString(operation));
            auto plan =
                BuildSubaruDensoSh7058CanPlan(operation, variant.protocol, "SH7058", ImageFor(operation), Kernel());

            ASSERT_THAT(plan, fastecu::testing::IsOk()) << variant.protocol;
            EXPECT_EQ(plan->Family(), FlashFamily::kSubaruDensoSh7058Can);
            EXPECT_EQ(plan->Transport(), TransportKind::kCanIso15765);
            EXPECT_EQ(plan->TargetId(), variant.protocol);
            EXPECT_EQ(plan->McuName(), "SH7058");
            EXPECT_EQ(plan->TransferRegion().start, 0U);
            EXPECT_EQ(plan->TransferRegion().length, kRomSize);
            EXPECT_THAT(plan->Confirmations(), IsEmpty());
            ASSERT_THAT(plan->Kernel(), Optional(_));
            EXPECT_EQ(plan->Kernel()->id, "petrol-sh7058-kernel");
            EXPECT_EQ(plan->Kernel()->load_address, kKernelAddress);

            const auto& wire = plan->FamilyPlan().Get<SubaruDensoSh7058CanPlan>();
            EXPECT_EQ(wire.request_id, 0x7E0U);
            EXPECT_EQ(wire.response_id, 0x7E8U);
            EXPECT_EQ(wire.bitrate, 500000);
            EXPECT_FALSE(wire.extended_id);
            EXPECT_EQ(wire.security, variant.security);

            if (operation == FlashOperation::kRead)
            {
                EXPECT_THAT(plan->Image(), Eq(std::nullopt));
                EXPECT_THAT(plan->EraseRegions(), IsEmpty());
            }
            else
            {
                ASSERT_THAT(plan->Image(), Optional(_));
                EXPECT_EQ(plan->Image()->size(), kRomSize);
                ASSERT_EQ(plan->EraseRegions().size(), kSh7058Blocks.size());
                for (std::size_t block = 0; block < kSh7058Blocks.size(); ++block)
                {
                    SCOPED_TRACE(::testing::PrintToString(block));
                    EXPECT_EQ(plan->EraseRegions()[block].start, kSh7058Blocks[block].start);
                    EXPECT_EQ(plan->EraseRegions()[block].length, kSh7058Blocks[block].length);
                }
            }
            EXPECT_THAT(ValidateSubaruDensoSh7058CanPlan(*plan), fastecu::testing::IsOk());
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
        SCOPED_TRACE(::testing::PrintToString(protocol));
        auto plan = BuildSubaruDensoSh7058CanPlan(FlashOperation::kRead, protocol, "SH7058", std::nullopt, Kernel());
        ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig)) << protocol;
    }

    for (const Variant& variant : kVariants)
    {
        SCOPED_TRACE(::testing::PrintToString(variant));
        auto plan =
            BuildSubaruDensoSh7058CanPlan(FlashOperation::kRead, variant.protocol, "SH7058d", std::nullopt, Kernel());
        EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig)) << variant.protocol;
    }
}

TEST(SubaruDensoSh7058CanPlan, EnforcesReadAndWriteImageRequirementsExactly)
{
    const Variant& stock = kVariants.front();
    auto read_with_image = BuildSubaruDensoSh7058CanPlan(FlashOperation::kRead, stock.protocol, "SH7058",
                                                         bytes::Bytes(kRomSize, 0), Kernel());
    EXPECT_THAT(read_with_image, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));

    for (const FlashOperation operation : {FlashOperation::kTestWrite, FlashOperation::kWrite})
    {
        SCOPED_TRACE(::testing::PrintToString(operation));
        const std::array<std::optional<bytes::Bytes>, 3> invalid{{
            std::nullopt,
            std::optional<bytes::Bytes>{bytes::Bytes(kRomSize - 1, 0)},
            std::optional<bytes::Bytes>{bytes::Bytes(kRomSize + 1, 0)},
        }};
        for (const auto& image : invalid)
        {
            SCOPED_TRACE(::testing::PrintToString(image));
            auto plan = BuildSubaruDensoSh7058CanPlan(operation, stock.protocol, "SH7058", image, Kernel());
            EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
        }
    }
}

TEST(SubaruDensoSh7058CanPlan, EnforcesKernelIdentityAddressAnd128BytePaddedContainingRegion)
{
    const Variant& stock = kVariants.front();
    auto exact = BuildSubaruDensoSh7058CanPlan(FlashOperation::kRead, stock.protocol, "SH7058", std::nullopt,
                                               Kernel(bytes::Bytes(kKernelRegionSize, bytes::Byte{0xA5})));
    EXPECT_THAT(exact, fastecu::testing::IsOk());

    for (KernelImage invalid : {
             KernelImage{.id = "", .load_address = kKernelAddress, .bytes = {0x01}},
             KernelImage{.id = "wrong-address", .load_address = kKernelAddress + 1, .bytes = {0x01}},
             KernelImage{.id = "empty", .load_address = kKernelAddress, .bytes = {}},
             Kernel(bytes::Bytes(kKernelRegionSize + 1, bytes::Byte{0xA5})),
         })
    {
        SCOPED_TRACE(::testing::PrintToString(invalid));
        auto plan = BuildSubaruDensoSh7058CanPlan(FlashOperation::kRead, stock.protocol, "SH7058", std::nullopt,
                                                  std::move(invalid));
        EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    }
}

TEST(SubaruDensoSh7058CanPlan, ValidatorRejectsWireSecurityRegionGeometryAndConfirmationDrift)
{
    for (int mutation = 0; mutation < 9; ++mutation)
    {
        SCOPED_TRACE(::testing::PrintToString(mutation));
        auto fields = ValidFields(kVariants.front());
        auto& wire = fields.family_plan.Get<SubaruDensoSh7058CanPlan>();
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
            wire.security = SubaruDensoSh7058CanSecurity::kEcuTek;
            break;
        case 5:
            fields.transfer_region.length -= 1;
            break;
        case 6:
            fields.erase_regions.pop_back();
            break;
        case 7:
            fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}};
            break;
        case 8:
            // Security is carried by the validated enum, never inferred from
            // a mutable target-id suffix.
            fields.target_id = "sub_ecu_denso_sh7058_can_ecutek";
            break;
        default:
            FAIL() << "unexpected validator mutation " << mutation;
        }
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_THAT(plan, fastecu::testing::IsOk());
        auto valid = ValidateSubaruDensoSh7058CanPlan(*plan);
        EXPECT_THAT(valid, fastecu::testing::IsErr(ErrorKind::kInvalidConfig)) << mutation;
    }
}

TEST(SubaruDensoSh7058CanPlan, RejectsAlteredAddressesAndLengthsInEveryEraseBlock)
{
    for (unsigned index = 0; index < 16; ++index)
    {
        SCOPED_TRACE(::testing::PrintToString(index));
        for (bool alter_start : {false, true})
        {
            SCOPED_TRACE(::testing::PrintToString(alter_start));
            auto fields = ValidFields(kVariants.front());
            if (alter_start)
            {
                ++fields.erase_regions[index].start;
            }
            else
            {
                --fields.erase_regions[index].length;
            }
            auto plan = ValidateAndBuild(std::move(fields));
            ASSERT_THAT(plan, fastecu::testing::IsOk());
            const auto validation = ValidateSubaruDensoSh7058CanPlan(*plan);
            EXPECT_THAT(validation,
                        fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                                    ::testing::StrEq("erase geometry does not match the MCU")))
                << index;
        }
    }
}

} // namespace
} // namespace fastecu::flash
