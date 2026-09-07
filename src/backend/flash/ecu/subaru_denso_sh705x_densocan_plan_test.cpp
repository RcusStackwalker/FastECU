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

KernelImage kernel_for(const Case& test_case, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04, 0x05})
{
    return {.id = "densocan-kernel", .load_address = test_case.kernel_load_address, .bytes = std::move(data)};
}

std::optional<bytes::Bytes> image_for(const Case& test_case, FlashOperation operation)
{
    if (operation == FlashOperation::Read)
    {
        return std::nullopt;
    }
    return bytes::Bytes(test_case.rom_size, bytes::Byte{0});
}

FlashPlanFields valid_fields(const Case& test_case, FlashOperation operation = FlashOperation::Read)
{
    return {
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh705xDensoCan,
        .transport = TransportKind::CanRawIso15765,
        .target_id = std::string(test_case.protocol),
        .mcu_name = std::string(test_case.mcu),
        .transfer_region = {0x00000000, static_cast<std::uint32_t>(test_case.rom_size)},
        .erase_regions = operation == FlashOperation::Read
                             ? std::vector<MemoryRegion>{}
                             : std::vector<MemoryRegion>(test_case.blocks.begin(), test_case.blocks.end()),
        .image = image_for(test_case, operation),
        .kernel = kernel_for(test_case),
        .family_plan = SubaruDensoSh705xDensoCanPlan{},
        .confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}},
    };
}

TEST(SubaruDensoSh705xDensoCanPlan, BuildsEveryExactProtocolMcuOperationAndGeometry)
{
    for (const Case& test_case : kCases)
    {
        for (const FlashOperation operation : {FlashOperation::Read, FlashOperation::TestWrite, FlashOperation::Write})
        {
            auto plan = build_subaru_denso_sh705x_densocan_plan(operation, test_case.protocol, test_case.mcu,
                                                                image_for(test_case, operation), kernel_for(test_case));

            ASSERT_TRUE(plan.has_value()) << test_case.protocol << ": " << plan.error().detail;
            EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoSh705xDensoCan);
            EXPECT_EQ(plan->transport(), TransportKind::CanRawIso15765);
            EXPECT_EQ(plan->target_id(), test_case.protocol);
            EXPECT_EQ(plan->mcu_name(), test_case.mcu);
            EXPECT_EQ(plan->transfer_region().start, 0U);
            EXPECT_EQ(plan->transfer_region().length, test_case.rom_size);
            ASSERT_TRUE(plan->kernel().has_value());
            EXPECT_EQ(plan->kernel()->load_address, test_case.kernel_load_address);
            ASSERT_EQ(plan->confirmations().size(), 1U);
            EXPECT_EQ(plan->confirmations().front().id, ConfirmationSpec::Id::CycleIgnition);
            EXPECT_TRUE(plan->confirmations().front().arguments.empty());

            const auto& wire = std::get<SubaruDensoSh705xDensoCanPlan>(plan->family_plan());
            EXPECT_EQ(wire.iso_request_id, 0x7E0U);
            EXPECT_EQ(wire.iso_response_id, 0x7E8U);
            EXPECT_EQ(wire.raw_transmit_id, 0x000FFFFEU);
            EXPECT_EQ(wire.raw_receive_id, 0x21U);
            EXPECT_EQ(wire.bitrate, 500000);
            EXPECT_FALSE(wire.iso_extended_id);
            EXPECT_TRUE(wire.raw_extended_id);

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
            EXPECT_TRUE(validate_subaru_denso_sh705x_densocan_plan(*plan).has_value());
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
            auto plan = build_subaru_denso_sh705x_densocan_plan(FlashOperation::Read, bad_protocol, test_case.mcu,
                                                                std::nullopt, kernel_for(test_case));
            ASSERT_FALSE(plan.has_value()) << bad_protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
        }

        const std::string_view wrong_mcu = test_case.mcu == "SH7055" ? "SH7058" : "SH7055";
        auto plan = build_subaru_denso_sh705x_densocan_plan(FlashOperation::Read, test_case.protocol, wrong_mcu,
                                                            std::nullopt, kernel_for(test_case));
        ASSERT_FALSE(plan.has_value()) << test_case.protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoSh705xDensoCanPlan, WriteAndTestWriteRequireExactCatalogRomSizes)
{
    for (const Case& test_case : kCases)
    {
        for (const FlashOperation operation : {FlashOperation::TestWrite, FlashOperation::Write})
        {
            const std::array<std::optional<bytes::Bytes>, 3> invalid_images{{
                std::nullopt,
                std::optional<bytes::Bytes>{bytes::Bytes(test_case.rom_size - 1, 0)},
                std::optional<bytes::Bytes>{bytes::Bytes(test_case.rom_size + 1, 0)},
            }};
            for (const std::optional<bytes::Bytes>& image : invalid_images)
            {
                auto plan = build_subaru_denso_sh705x_densocan_plan(operation, test_case.protocol, test_case.mcu, image,
                                                                    kernel_for(test_case));
                ASSERT_FALSE(plan.has_value()) << test_case.protocol;
                EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
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

        auto exact = build_subaru_denso_sh705x_densocan_plan(
            FlashOperation::Read, test_case.protocol, test_case.mcu, std::nullopt,
            kernel_for(test_case, bytes::Bytes(capacity, bytes::Byte{0xA5})));
        ASSERT_TRUE(exact.has_value()) << test_case.protocol << ": " << exact.error().detail;

        for (KernelImage kernel : {
                 KernelImage{.id = "wrong-address", .load_address = test_case.kernel_load_address + 1, .bytes = {0xAA}},
                 kernel_for(test_case, bytes::Bytes(capacity + 1, bytes::Byte{0xA5})),
             })
        {
            auto plan = build_subaru_denso_sh705x_densocan_plan(FlashOperation::Read, test_case.protocol, test_case.mcu,
                                                                std::nullopt, std::move(kernel));
            ASSERT_FALSE(plan.has_value()) << test_case.protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
        }
    }
}

TEST(SubaruDensoSh705xDensoCanPlan, ValidatorRejectsWireGeometryAndConfirmationDrift)
{
    const Case& test_case = kCases.front();
    for (int mutation = 0; mutation < 6; ++mutation)
    {
        auto fields = valid_fields(test_case, FlashOperation::Write);
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
        }
        auto plan = validate_and_build(std::move(fields));
        ASSERT_TRUE(plan.has_value()) << plan.error().detail;
        auto valid = validate_subaru_denso_sh705x_densocan_plan(*plan);
        ASSERT_FALSE(valid.has_value()) << mutation;
        EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
    }
}

} // namespace
} // namespace fastecu::flash
