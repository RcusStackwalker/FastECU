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

KernelImage kernel_for(const Case& test_case, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = "tcu-denso-kernel", .load_address = test_case.kernel_load_address, .bytes = std::move(data)};
}

std::optional<bytes::Bytes> image_for(const Case& test_case, FlashOperation operation)
{
    if (operation == FlashOperation::Read)
    {
        return std::nullopt;
    }
    return bytes::Bytes(test_case.rom_size, bytes::Byte{0});
}

FlashPlanFields valid_fields(const Case& test_case, FlashOperation operation)
{
    return {
        .operation = operation,
        .family = FlashFamily::SubaruTcuDensoSh705xCan,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(test_case.protocol),
        .mcu_name = std::string(test_case.mcu),
        .transfer_region = {0x00000000, static_cast<std::uint32_t>(test_case.rom_size)},
        .erase_regions = operation == FlashOperation::Write
                             ? std::vector<MemoryRegion>(test_case.blocks.begin(), test_case.blocks.end())
                             : std::vector<MemoryRegion>{},
        .image = image_for(test_case, operation),
        .kernel = kernel_for(test_case),
        .family_plan = SubaruTcuDensoSh705xCanPlan{},
        .confirmations = {},
    };
}

TEST(SubaruTcuDensoSh705xCanPlan, BuildsExactCapabilitiesWireAndGeometry)
{
    for (const Case& test_case : kCases)
    {
        const std::array<FlashOperation, 2> operations = test_case.supports_write
                                                             ? std::array{FlashOperation::Read, FlashOperation::Write}
                                                             : std::array{FlashOperation::Read, FlashOperation::Read};
        const std::size_t operation_count = test_case.supports_write ? 2U : 1U;
        for (std::size_t index = 0; index < operation_count; ++index)
        {
            const FlashOperation operation = operations[index];
            auto plan = build_subaru_tcu_denso_sh705x_can_plan(operation, test_case.protocol, test_case.mcu,
                                                               image_for(test_case, operation), kernel_for(test_case));

            ASSERT_TRUE(plan.has_value()) << test_case.protocol << ": " << plan.error().detail;
            EXPECT_EQ(plan->family(), FlashFamily::SubaruTcuDensoSh705xCan);
            EXPECT_EQ(plan->transport(), TransportKind::CanIso15765);
            EXPECT_EQ(plan->target_id(), test_case.protocol);
            EXPECT_EQ(plan->mcu_name(), test_case.mcu);
            EXPECT_EQ(plan->transfer_region().start, 0U);
            EXPECT_EQ(plan->transfer_region().length, test_case.rom_size);
            EXPECT_TRUE(plan->confirmations().empty());
            ASSERT_TRUE(plan->kernel().has_value());
            EXPECT_EQ(plan->kernel()->load_address, test_case.kernel_load_address);

            const auto& wire = std::get<SubaruTcuDensoSh705xCanPlan>(plan->family_plan());
            EXPECT_EQ(wire.request_id, 0x7E1U);
            EXPECT_EQ(wire.response_id, 0x7E9U);
            EXPECT_EQ(wire.bitrate, 500000);
            EXPECT_FALSE(wire.extended_id);

            if (operation == FlashOperation::Read)
            {
                EXPECT_FALSE(plan->image().has_value());
                EXPECT_TRUE(plan->erase_regions().empty());
            }
            else
            {
                ASSERT_TRUE(plan->image().has_value());
                EXPECT_EQ(plan->image()->size(), test_case.rom_size);
                ASSERT_EQ(plan->erase_regions().size(), test_case.blocks.size());
                for (std::size_t block = 0; block < test_case.blocks.size(); ++block)
                {
                    EXPECT_EQ(plan->erase_regions()[block].start, test_case.blocks[block].start);
                    EXPECT_EQ(plan->erase_regions()[block].length, test_case.blocks[block].length);
                }
            }
            EXPECT_TRUE(validate_subaru_tcu_denso_sh705x_can_plan(*plan).has_value());
        }
    }
}

TEST(SubaruTcuDensoSh705xCanPlan, RejectsUnsupportedOperationsBeforeImageOrKernelValidation)
{
    const Case& sh7055 = kCases[0];
    const Case& sh7058 = kCases[1];

    for (const FlashOperation operation : {FlashOperation::Write, FlashOperation::TestWrite})
    {
        auto rejected = build_subaru_tcu_denso_sh705x_can_plan(
            operation, sh7055.protocol, sh7055.mcu, std::nullopt,
            KernelImage{.id = "", .load_address = sh7055.kernel_load_address + 1, .bytes = {}});
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(rejected.error().kind, ErrorKind::Unsupported);
    }

    auto sh7058_test_write =
        build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::TestWrite, sh7058.protocol, sh7058.mcu,
                                               image_for(sh7058, FlashOperation::Write), kernel_for(sh7058));
    ASSERT_FALSE(sh7058_test_write.has_value());
    EXPECT_EQ(sh7058_test_write.error().kind, ErrorKind::Unsupported);
}

TEST(SubaruTcuDensoSh705xCanPlan, RejectsUnknownNearMissAndWrongMcuIdentities)
{
    for (const Case& test_case : kCases)
    {
        for (const std::string_view bad_protocol :
             {"unknown_tcu_denso_can", "sub_tcu_denso_sh7058_can_typo", "sub_tcu_denso_sh7055_can_extra"})
        {
            auto plan = build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Read, bad_protocol, test_case.mcu,
                                                               std::nullopt, kernel_for(test_case));
            ASSERT_FALSE(plan.has_value()) << bad_protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
        }
        const std::string_view wrong_mcu = test_case.mcu == "SH7055" ? "SH7058" : "SH7055";
        auto plan = build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Read, test_case.protocol, wrong_mcu,
                                                           std::nullopt, kernel_for(test_case));
        ASSERT_FALSE(plan.has_value()) << test_case.protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruTcuDensoSh705xCanPlan, EnforcesExactImagePresenceAndRomSize)
{
    const Case& sh7058 = kCases[1];
    for (const std::optional<bytes::Bytes>& bad_image :
         {std::optional<bytes::Bytes>{}, std::optional<bytes::Bytes>{bytes::Bytes(sh7058.rom_size - 1, 0)},
          std::optional<bytes::Bytes>{bytes::Bytes(sh7058.rom_size + 1, 0)}})
    {
        auto plan = build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Write, sh7058.protocol, sh7058.mcu,
                                                           bad_image, kernel_for(sh7058));
        ASSERT_FALSE(plan.has_value());
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }

    auto read_with_image = build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Read, sh7058.protocol, sh7058.mcu,
                                                                  bytes::Bytes(sh7058.rom_size, 0), kernel_for(sh7058));
    ASSERT_FALSE(read_with_image.has_value());
    EXPECT_EQ(read_with_image.error().kind, ErrorKind::InvalidConfig);
}

TEST(SubaruTcuDensoSh705xCanPlan, EnforcesKernelIdentityAddressAnd128BytePaddedBounds)
{
    for (const Case& test_case : kCases)
    {
        const std::size_t capacity = static_cast<std::size_t>(test_case.kernel_region_start) +
                                     test_case.kernel_region_size - test_case.kernel_load_address;
        const std::size_t padded_capacity = capacity / 128U * 128U;
        ASSERT_GT(padded_capacity, 0U);

        auto exact = build_subaru_tcu_denso_sh705x_can_plan(
            FlashOperation::Read, test_case.protocol, test_case.mcu, std::nullopt,
            kernel_for(test_case, bytes::Bytes(padded_capacity, bytes::Byte{0xA5})));
        ASSERT_TRUE(exact.has_value()) << exact.error().detail;

        for (KernelImage kernel : {
                 KernelImage{.id = "", .load_address = test_case.kernel_load_address, .bytes = {0x01}},
                 KernelImage{.id = "wrong-address", .load_address = test_case.kernel_load_address + 1, .bytes = {0x01}},
                 kernel_for(test_case, bytes::Bytes(padded_capacity + 1, bytes::Byte{0xA5})),
             })
        {
            auto plan = build_subaru_tcu_denso_sh705x_can_plan(FlashOperation::Read, test_case.protocol, test_case.mcu,
                                                               std::nullopt, std::move(kernel));
            ASSERT_FALSE(plan.has_value()) << test_case.protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
        }
    }
}

TEST(SubaruTcuDensoSh705xCanPlan, ValidatorRejectsWireRegionGeometryAndConfirmationDrift)
{
    const Case& sh7058 = kCases[1];
    for (int mutation = 0; mutation < 6; ++mutation)
    {
        auto fields = valid_fields(sh7058, FlashOperation::Write);
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
            fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}};
            break;
        }
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto valid = validate_subaru_tcu_denso_sh705x_can_plan(*plan);
        ASSERT_FALSE(valid.has_value()) << mutation;
        EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
    }
}

} // namespace
} // namespace fastecu::flash
