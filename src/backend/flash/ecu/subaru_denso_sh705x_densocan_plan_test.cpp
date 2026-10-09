#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

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

constexpr std::array<MemoryRegion, 16> kSh7059dBlocks{{
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
    {0x00080000, 0x00040000},
    {0x000C0000, 0x00040000},
    {0x00100000, 0x00040000},
    {0x00140000, 0x00040000},
}};

struct Case
{
    std::string_view protocol;
    std::string_view mcu;
    std::size_t rom_size;
    std::uint32_t kernel_load_address;
    std::uint32_t kernel_region_start;
    std::size_t kernel_region_size;
    const std::array<MemoryRegion, 16>& blocks;
};

constexpr std::array<Case, 5> kCases{{
    {"sub_ecu_denso_sh7055_densocan", "SH7055", 0x80000, 0xFFFF6004, 0xFFFF6004, 0x6000, kSh7055Blocks},
    {"sub_ecu_denso_sh7058_densocan", "SH7058", 0x100000, 0xFFFF3000, 0xFFFF3000, 0x9000, kSh7058Blocks},
    {"sub_ecu_denso_sh7058s_densocan", "SH7058", 0x100000, 0xFFFF3000, 0xFFFF3000, 0x9000, kSh7058Blocks},
    {"sub_ecu_denso_sh7058s_diesel_densocan", "SH7058", 0x100000, 0xFFFF3000, 0xFFFF3000, 0x9000, kSh7058Blocks},
    {"sub_ecu_denso_sh7059_diesel_densocan", "SH7059d", 0x180000, 0xFFFEE000, 0xFFFE8000, 0x9000, kSh7059dBlocks},
}};

KernelImage KernelFor(const Case& test_case, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04, 0x05})
{
    return {.id = "densocan-kernel", .load_address = test_case.kernel_load_address, .bytes = std::move(data)};
}

std::optional<bytes::Bytes> ImageFor(const Case& test_case, FlashOperation operation)
{
    if (operation == FlashOperation::kRead)
    {
        return std::nullopt;
    }
    return bytes::Bytes(test_case.rom_size, bytes::Byte{0});
}

FlashPlanFields ValidFields(const Case& test_case, FlashOperation operation = FlashOperation::kRead)
{
    return {
        .operation = operation,
        .family = FlashFamily::kSubaruDensoSh705xDensoCan,
        .transport = TransportKind::kCanRawIso15765,
        .target_id = std::string(test_case.protocol),
        .mcu_name = std::string(test_case.mcu),
        .transfer_region = {0x00000000, static_cast<std::uint32_t>(test_case.rom_size)},
        .erase_regions = operation == FlashOperation::kRead
                             ? std::vector<MemoryRegion>{}
                             : std::vector<MemoryRegion>(test_case.blocks.begin(), test_case.blocks.end()),
        .image = ImageFor(test_case, operation),
        .kernel = KernelFor(test_case),
        .family_plan = SubaruDensoSh705xDensoCanPlan{},
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}},
    };
}

TEST(SubaruDensoSh705xDensoCanPlan, BuildsEveryExactProtocolMcuOperationAndGeometry)
{
    for (const Case& test_case : kCases)
    {
        for (const FlashOperation operation :
             {FlashOperation::kRead, FlashOperation::kTestWrite, FlashOperation::kWrite})
        {
            auto plan = BuildSubaruDensoSh705xDensocanPlan(operation, test_case.protocol, test_case.mcu,
                                                           ImageFor(test_case, operation), KernelFor(test_case));

            ASSERT_TRUE(plan.has_value()) << test_case.protocol << ": " << plan.error().detail;
            EXPECT_EQ(plan->Family(), FlashFamily::kSubaruDensoSh705xDensoCan);
            EXPECT_EQ(plan->Transport(), TransportKind::kCanRawIso15765);
            EXPECT_EQ(plan->TargetId(), test_case.protocol);
            EXPECT_EQ(plan->McuName(), test_case.mcu);
            EXPECT_EQ(plan->TransferRegion().start, 0U);
            EXPECT_EQ(plan->TransferRegion().length, test_case.rom_size);
            ASSERT_TRUE(plan->Kernel().has_value());
            EXPECT_EQ(plan->Kernel()->load_address, test_case.kernel_load_address);
            ASSERT_EQ(plan->Confirmations().size(), 1U);
            EXPECT_EQ(plan->Confirmations().front().id, ConfirmationSpec::Id::kCycleIgnition);
            EXPECT_TRUE(plan->Confirmations().front().arguments.empty());

            const auto& wire = std::get<SubaruDensoSh705xDensoCanPlan>(plan->FamilyPlan());
            EXPECT_EQ(wire.iso_request_id, 0x7E0U);
            EXPECT_EQ(wire.iso_response_id, 0x7E8U);
            EXPECT_EQ(wire.raw_transmit_id, 0x000FFFFEU);
            EXPECT_EQ(wire.raw_receive_id, 0x21U);
            EXPECT_EQ(wire.bitrate, 500000);
            EXPECT_FALSE(wire.iso_extended_id);
            EXPECT_TRUE(wire.raw_extended_id);

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
            EXPECT_TRUE(ValidateSubaruDensoSh705xDensocanPlan(*plan).has_value());
        }
    }
}

TEST(SubaruDensoSh705xDensoCanPlan, RejectsUnknownNearMissAndWrongMcuPairsBeforeTransportExists)
{
    for (const Case& test_case : kCases)
    {
        for (const std::string_view bad_protocol :
             {"unknown_densocan", "sub_ecu_denso_sh7058_densocan_extra", "sub_ecu_eeprom_denso_sh7058_densocan"})
        {
            auto plan = BuildSubaruDensoSh705xDensocanPlan(FlashOperation::kRead, bad_protocol, test_case.mcu,
                                                           std::nullopt, KernelFor(test_case));
            ASSERT_FALSE(plan.has_value()) << bad_protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
        }

        const std::string_view wrong_mcu = test_case.mcu == "SH7055" ? "SH7058" : "SH7055";
        auto plan = BuildSubaruDensoSh705xDensocanPlan(FlashOperation::kRead, test_case.protocol, wrong_mcu,
                                                       std::nullopt, KernelFor(test_case));
        ASSERT_FALSE(plan.has_value()) << test_case.protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
    }
}

TEST(SubaruDensoSh705xDensoCanPlan, WriteAndTestWriteRequireExactCatalogRomSizes)
{
    for (const Case& test_case : kCases)
    {
        for (const FlashOperation operation : {FlashOperation::kTestWrite, FlashOperation::kWrite})
        {
            const std::array<std::optional<bytes::Bytes>, 3> invalid_images{{
                std::nullopt,
                std::optional<bytes::Bytes>{bytes::Bytes(test_case.rom_size - 1, 0)},
                std::optional<bytes::Bytes>{bytes::Bytes(test_case.rom_size + 1, 0)},
            }};
            for (const std::optional<bytes::Bytes>& image : invalid_images)
            {
                auto plan = BuildSubaruDensoSh705xDensocanPlan(operation, test_case.protocol, test_case.mcu, image,
                                                               KernelFor(test_case));
                ASSERT_FALSE(plan.has_value()) << test_case.protocol;
                EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
            }
        }
    }
}

TEST(SubaruDensoSh705xDensoCanPlan, KernelUploadRequiresExactCatalogAddressAndSixBytePaddedFit)
{
    for (const Case& test_case : kCases)
    {
        const std::size_t capacity = static_cast<std::size_t>(test_case.kernel_region_start) +
                                     test_case.kernel_region_size - test_case.kernel_load_address;
        ASSERT_GT(capacity, 0U);
        ASSERT_EQ(capacity % 6U, 0U);

        auto exact =
            BuildSubaruDensoSh705xDensocanPlan(FlashOperation::kRead, test_case.protocol, test_case.mcu, std::nullopt,
                                               KernelFor(test_case, bytes::Bytes(capacity, bytes::Byte{0xA5})));
        ASSERT_TRUE(exact.has_value()) << test_case.protocol << ": " << exact.error().detail;

        for (KernelImage kernel : {
                 KernelImage{.id = "wrong-address", .load_address = test_case.kernel_load_address + 1, .bytes = {0xAA}},
                 KernelFor(test_case, bytes::Bytes(capacity + 1, bytes::Byte{0xA5})),
             })
        {
            auto plan = BuildSubaruDensoSh705xDensocanPlan(FlashOperation::kRead, test_case.protocol, test_case.mcu,
                                                           std::nullopt, std::move(kernel));
            ASSERT_FALSE(plan.has_value()) << test_case.protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
        }
    }
}

TEST(SubaruDensoSh705xDensoCanPlan, ValidatorRejectsWireGeometryAndConfirmationDrift)
{
    const Case& test_case = kCases.front();
    for (int mutation = 0; mutation < 6; ++mutation)
    {
        auto fields = ValidFields(test_case, FlashOperation::kWrite);
        auto& wire = std::get<SubaruDensoSh705xDensoCanPlan>(fields.family_plan);
        switch (mutation)
        {
        case 0:
            wire.iso_request_id = 0x7E1;
            break;
        case 1:
            wire.iso_extended_id = true;
            break;
        case 2:
            wire.raw_receive_id = 0x22;
            break;
        case 3:
            fields.transfer_region.length -= 1;
            break;
        case 4:
            fields.erase_regions.pop_back();
            break;
        case 5:
            fields.confirmations.clear();
            break;
        default:
            FAIL() << "unexpected validator mutation " << mutation;
        }
        auto plan = ValidateAndBuild(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto valid = ValidateSubaruDensoSh705xDensocanPlan(*plan);
        ASSERT_FALSE(valid.has_value()) << mutation;
        EXPECT_EQ(valid.error().kind, ErrorKind::kInvalidConfig);
    }
}

TEST(SubaruDensoSh705xDensoCanPlan, RejectsAlteredAddressesAndLengthsInEveryEraseBlock)
{
    for (unsigned index = 0; index < 16; ++index)
    {
        for (bool alter_start : {false, true})
        {
            auto fields = ValidFields(kCases.front(), FlashOperation::kWrite);
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
            const auto validation = ValidateSubaruDensoSh705xDensocanPlan(*plan);
            ASSERT_FALSE(validation.has_value()) << index;
            EXPECT_EQ(validation.error(), (Error{ErrorKind::kInvalidConfig, "erase geometry does not match the MCU"}));
        }
    }
}

} // namespace
} // namespace fastecu::flash
