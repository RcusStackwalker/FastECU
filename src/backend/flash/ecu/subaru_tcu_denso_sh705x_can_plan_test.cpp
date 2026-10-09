#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"

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

// Independently transcribed from kernelmemorymodels.h:163-168 and 209-214;
// do not derive expectations from the flash-device table under test.
constexpr std::array<MemoryRegion, 16> kSh7055Blocks{{
    {0x00000000, 0x00001000},
    {0x00001000, 0x00001000},
    {0x00002000, 0x00001000},
    {0x00003000, 0x00001000},
    {0x00004000, 0x00001000},
    {0x00005000, 0x00001000},
    {0x00006000, 0x00001000},
    {0x00007000, 0x00001000},
    {0x00008000, 0x00008000},
    {0x00010000, 0x00010000},
    {0x00020000, 0x00010000},
    {0x00030000, 0x00010000},
    {0x00040000, 0x00010000},
    {0x00050000, 0x00010000},
    {0x00060000, 0x00010000},
    {0x00070000, 0x00010000},
}};

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

struct Case
{
    std::string_view protocol;
    std::string_view mcu;
    std::size_t rom_size;
    std::uint32_t kernel_load_address;
    std::uint32_t kernel_region_start;
    std::size_t kernel_region_size;
    bool supports_write;
    const std::array<MemoryRegion, 16>& blocks;
};

constexpr std::array<Case, 2> kCases{{
    {"sub_tcu_denso_sh7055_can", "SH7055", 0x00080000, 0xFFFF9000, 0xFFFF6004, 0x00006000, false, kSh7055Blocks},
    {"sub_tcu_denso_sh7058_can", "SH7058", 0x00100000, 0xFFFF3000, 0xFFFF3000, 0x00009000, true, kSh7058Blocks},
}};

KernelImage KernelFor(const Case& test_case, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = "tcu-denso-kernel", .load_address = test_case.kernel_load_address, .bytes = std::move(data)};
}

std::optional<bytes::Bytes> ImageFor(const Case& test_case, FlashOperation operation)
{
    if (operation == FlashOperation::kRead)
    {
        return std::nullopt;
    }
    return bytes::Bytes(test_case.rom_size, bytes::Byte{0});
}

FlashPlanFields ValidFields(const Case& test_case, FlashOperation operation)
{
    return {
        .operation = operation,
        .family = FlashFamily::kSubaruTcuDensoSh705xCan,
        .transport = TransportKind::kCanIso15765,
        .target_id = std::string(test_case.protocol),
        .mcu_name = std::string(test_case.mcu),
        .transfer_region = {0x00000000, static_cast<std::uint32_t>(test_case.rom_size)},
        .erase_regions = operation == FlashOperation::kWrite
                             ? std::vector<MemoryRegion>(test_case.blocks.begin(), test_case.blocks.end())
                             : std::vector<MemoryRegion>{},
        .image = ImageFor(test_case, operation),
        .kernel = KernelFor(test_case),
        .family_plan = SubaruTcuDensoSh705xCanPlan{},
        .confirmations = {},
    };
}

TEST(SubaruTcuDensoSh705xCanPlan, BuildsExactCapabilitiesWireAndGeometry)
{
    for (const Case& test_case : kCases)
    {
        const std::array<FlashOperation, 2> operations = test_case.supports_write
                                                             ? std::array{FlashOperation::kRead, FlashOperation::kWrite}
                                                             : std::array{FlashOperation::kRead, FlashOperation::kRead};
        const std::size_t operation_count = test_case.supports_write ? 2U : 1U;
        for (std::size_t index = 0; index < operation_count; ++index)
        {
            const FlashOperation operation = operations[index];
            auto plan = BuildSubaruTcuDensoSh705xCanPlan(operation, test_case.protocol, test_case.mcu,
                                                         ImageFor(test_case, operation), KernelFor(test_case));

            ASSERT_TRUE(plan.has_value()) << test_case.protocol << ": " << plan.error().detail;
            EXPECT_EQ(plan->Family(), FlashFamily::kSubaruTcuDensoSh705xCan);
            EXPECT_EQ(plan->Transport(), TransportKind::kCanIso15765);
            EXPECT_EQ(plan->TargetId(), test_case.protocol);
            EXPECT_EQ(plan->McuName(), test_case.mcu);
            EXPECT_EQ(plan->TransferRegion().start, 0U);
            EXPECT_EQ(plan->TransferRegion().length, test_case.rom_size);
            EXPECT_TRUE(plan->Confirmations().empty());
            ASSERT_TRUE(plan->Kernel().has_value());
            EXPECT_EQ(plan->Kernel()->load_address, test_case.kernel_load_address);

            const auto& wire = std::get<SubaruTcuDensoSh705xCanPlan>(plan->FamilyPlan());
            EXPECT_EQ(wire.request_id, 0x7E1U);
            EXPECT_EQ(wire.response_id, 0x7E9U);
            EXPECT_EQ(wire.bitrate, 500000);
            EXPECT_FALSE(wire.extended_id);

            if (operation == FlashOperation::kRead)
            {
                EXPECT_FALSE(plan->Image().has_value());
                EXPECT_TRUE(plan->EraseRegions().empty());
            }
            else
            {
                ASSERT_TRUE(plan->Image().has_value());
                EXPECT_EQ(plan->Image()->size(), test_case.rom_size);
                ASSERT_EQ(plan->EraseRegions().size(), test_case.blocks.size());
                for (std::size_t block = 0; block < test_case.blocks.size(); ++block)
                {
                    EXPECT_EQ(plan->EraseRegions()[block].start, test_case.blocks[block].start);
                    EXPECT_EQ(plan->EraseRegions()[block].length, test_case.blocks[block].length);
                }
            }
            EXPECT_TRUE(ValidateSubaruTcuDensoSh705xCanPlan(*plan).has_value());
        }
    }
}

TEST(SubaruTcuDensoSh705xCanPlan, RejectsUnsupportedOperationsBeforeImageOrKernelValidation)
{
    const Case& sh7055 = kCases[0];
    const Case& sh7058 = kCases[1];

    for (const FlashOperation operation : {FlashOperation::kWrite, FlashOperation::kTestWrite})
    {
        auto rejected = BuildSubaruTcuDensoSh705xCanPlan(
            operation, sh7055.protocol, sh7055.mcu, std::nullopt,
            KernelImage{.id = "", .load_address = sh7055.kernel_load_address + 1, .bytes = {}});
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(rejected.error().kind, ErrorKind::kUnsupported);
    }

    auto sh7058_test_write =
        BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kTestWrite, sh7058.protocol, sh7058.mcu,
                                         ImageFor(sh7058, FlashOperation::kWrite), KernelFor(sh7058));
    ASSERT_FALSE(sh7058_test_write.has_value());
    EXPECT_EQ(sh7058_test_write.error().kind, ErrorKind::kUnsupported);
}

TEST(SubaruTcuDensoSh705xCanPlan, RejectsUnknownNearMissAndWrongMcuIdentities)
{
    for (const Case& test_case : kCases)
    {
        for (const std::string_view bad_protocol :
             {"unknown_tcu_denso_can", "sub_tcu_denso_sh7058_can_typo", "sub_tcu_denso_sh7055_can_extra"})
        {
            auto plan = BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kRead, bad_protocol, test_case.mcu,
                                                         std::nullopt, KernelFor(test_case));
            ASSERT_FALSE(plan.has_value()) << bad_protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
        }
        const std::string_view wrong_mcu = test_case.mcu == "SH7055" ? "SH7058" : "SH7055";
        auto plan = BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kRead, test_case.protocol, wrong_mcu, std::nullopt,
                                                     KernelFor(test_case));
        ASSERT_FALSE(plan.has_value()) << test_case.protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
    }
}

TEST(SubaruTcuDensoSh705xCanPlan, EnforcesExactImagePresenceAndRomSize)
{
    const Case& sh7058 = kCases[1];
    for (const std::optional<bytes::Bytes>& bad_image :
         {std::optional<bytes::Bytes>{}, std::optional<bytes::Bytes>{bytes::Bytes(sh7058.rom_size - 1, 0)},
          std::optional<bytes::Bytes>{bytes::Bytes(sh7058.rom_size + 1, 0)}})
    {
        auto plan = BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kWrite, sh7058.protocol, sh7058.mcu, bad_image,
                                                     KernelFor(sh7058));
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
    }

    auto read_with_image = BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kRead, sh7058.protocol, sh7058.mcu,
                                                            bytes::Bytes(sh7058.rom_size, 0), KernelFor(sh7058));
    ASSERT_FALSE(read_with_image.has_value());
    EXPECT_EQ(read_with_image.error().kind, ErrorKind::kInvalidConfig);
}

TEST(SubaruTcuDensoSh705xCanPlan, EnforcesKernelIdentityAddressAnd128BytePaddedBounds)
{
    for (const Case& test_case : kCases)
    {
        const std::size_t capacity = static_cast<std::size_t>(test_case.kernel_region_start) +
                                     test_case.kernel_region_size - test_case.kernel_load_address;
        const std::size_t padded_capacity = capacity / 128U * 128U;
        ASSERT_GT(padded_capacity, 0U);

        auto exact =
            BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kRead, test_case.protocol, test_case.mcu, std::nullopt,
                                             KernelFor(test_case, bytes::Bytes(padded_capacity, bytes::Byte{0xA5})));
        ASSERT_TRUE(exact.has_value()) << exact.error().detail;

        for (KernelImage kernel : {
                 KernelImage{.id = "", .load_address = test_case.kernel_load_address, .bytes = {0x01}},
                 KernelImage{.id = "wrong-address", .load_address = test_case.kernel_load_address + 1, .bytes = {0x01}},
                 KernelFor(test_case, bytes::Bytes(padded_capacity + 1, bytes::Byte{0xA5})),
             })
        {
            auto plan = BuildSubaruTcuDensoSh705xCanPlan(FlashOperation::kRead, test_case.protocol, test_case.mcu,
                                                         std::nullopt, std::move(kernel));
            ASSERT_FALSE(plan.has_value()) << test_case.protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
        }
    }
}

TEST(SubaruTcuDensoSh705xCanPlan, ValidatorRejectsWireRegionGeometryAndConfirmationDrift)
{
    const Case& sh7058 = kCases[1];
    for (int mutation = 0; mutation < 6; ++mutation)
    {
        auto fields = ValidFields(sh7058, FlashOperation::kWrite);
        auto& wire = std::get<SubaruTcuDensoSh705xCanPlan>(fields.family_plan);
        switch (mutation)
        {
        case 0:
            wire.request_id = 0x7E0;
            break;
        case 1:
            wire.extended_id = true;
            break;
        case 2:
            wire.response_id = 0x7E8;
            break;
        case 3:
            fields.transfer_region.length -= 1;
            break;
        case 4:
            fields.erase_regions.pop_back();
            break;
        case 5:
            fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}};
            break;
        default:
            FAIL() << "unexpected validator mutation " << mutation;
        }
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto valid = ValidateSubaruTcuDensoSh705xCanPlan(*plan);
        ASSERT_FALSE(valid.has_value()) << mutation;
        EXPECT_EQ(valid.error().kind, ErrorKind::kInvalidConfig);
    }
}

TEST(SubaruTcuDensoSh705xCanPlan, RejectsAlteredAddressesAndLengthsInEveryEraseBlock)
{
    for (unsigned index = 0; index < 16; ++index)
    {
        for (bool alter_start : {false, true})
        {
            auto fields = ValidFields(kCases[1], FlashOperation::kWrite);
            if (alter_start)
            {
                ++fields.erase_regions[index].start;
            }
            else
            {
                --fields.erase_regions[index].length;
            }
            auto plan = ValidateAndBuild(std::move(fields));
            ASSERT_TRUE(plan.has_value()) << plan.error().detail;
            const auto validation = ValidateSubaruTcuDensoSh705xCanPlan(*plan);
            ASSERT_FALSE(validation.has_value()) << index;
            EXPECT_EQ(validation.error(), (Error{ErrorKind::kInvalidConfig, "erase geometry does not match the MCU"}));
        }
    }
}

} // namespace
} // namespace fastecu::flash
