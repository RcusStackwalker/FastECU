#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/flash_validation.h"

namespace fastecu::flash
{
namespace
{

struct DieselCase
{
    std::string_view protocol;
    std::string_view mcu;
    std::size_t rom_size;
    std::uint32_t kernel_address;
    std::uint32_t kernel_region_start;
    std::uint32_t kernel_region_size;
};

constexpr std::array<DieselCase, 2> kDiesel{{
    {"sub_ecu_denso_sh7058_can_diesel", "SH7058d", 0x100000, 0xFFFF4000, 0xFFFF4000, 0x9000},
    {"sub_ecu_denso_sh7059_can_diesel", "SH7059d", 0x180000, 0xFFFEE000, 0xFFFE8000, 0x9000},
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

constexpr std::array<MemoryRegion, 16> kSh7059Blocks{{
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

const std::array<MemoryRegion, 16>& blocks_for(const DieselCase& diesel)
{
    return diesel.mcu == "SH7058d" ? kSh7058Blocks : kSh7059Blocks;
}

KernelImage kernel_for(const DieselCase& diesel, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = std::string(diesel.protocol) + "-kernel",
            .load_address = diesel.kernel_address,
            .bytes = std::move(data)};
}

std::optional<bytes::Bytes> image_for(const DieselCase& diesel, FlashOperation operation)
{
    if (operation == FlashOperation::Read)
    {
        return std::nullopt;
    }
    return bytes::Bytes(diesel.rom_size, bytes::Byte{0});
}

FlashPlanFields valid_fields(const DieselCase& diesel, FlashOperation operation = FlashOperation::Write)
{
    const auto& blocks = blocks_for(diesel);
    return {
        .operation = operation,
        .family = FlashFamily::SubaruDensoSh7058CanDiesel,
        .transport = TransportKind::CanIso15765,
        .target_id = std::string(diesel.protocol),
        .mcu_name = std::string(diesel.mcu),
        .transfer_region = {0, static_cast<std::uint32_t>(diesel.rom_size)},
        .erase_regions = operation == FlashOperation::Read ? std::vector<MemoryRegion>{}
                                                           : std::vector<MemoryRegion>(blocks.begin(), blocks.end()),
        .image = image_for(diesel, operation),
        .kernel = kernel_for(diesel),
        .family_plan = SubaruDensoSh7058CanDieselPlan{},
        .confirmations = {},
    };
}

TEST(SubaruDensoSh7058CanDieselPlan, BuildsBothExactGenerationsForEveryOperation)
{
    for (const DieselCase& diesel : kDiesel)
    {
        for (const FlashOperation operation : {FlashOperation::Read, FlashOperation::TestWrite, FlashOperation::Write})
        {
            const auto plan = build_subaru_denso_sh7058_can_diesel_plan(
                operation, diesel.protocol, diesel.mcu, image_for(diesel, operation), kernel_for(diesel));
            ASSERT_TRUE(plan.has_value()) << diesel.protocol << ": " << plan.error().detail;
            EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoSh7058CanDiesel);
            EXPECT_EQ(plan->transport(), TransportKind::CanIso15765);
            EXPECT_EQ(plan->target_id(), diesel.protocol);
            EXPECT_EQ(plan->mcu_name(), diesel.mcu);
            EXPECT_EQ(plan->transfer_region().start, 0U);
            EXPECT_EQ(plan->transfer_region().length, diesel.rom_size);
            EXPECT_TRUE(plan->confirmations().empty());
            ASSERT_TRUE(plan->kernel().has_value());
            EXPECT_EQ(plan->kernel()->load_address, diesel.kernel_address);
            EXPECT_EQ(plan->kernel()->id, std::string(diesel.protocol) + "-kernel");

            const auto& wire = std::get<SubaruDensoSh7058CanDieselPlan>(plan->family_plan());
            EXPECT_EQ(wire.request_id, 0x7E0U);
            EXPECT_EQ(wire.response_id, 0x7E8U);
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
                EXPECT_EQ(plan->image()->size(), diesel.rom_size);
                ASSERT_EQ(plan->erase_regions().size(), blocks_for(diesel).size());
                for (std::size_t index = 0; index < blocks_for(diesel).size(); ++index)
                {
                    EXPECT_EQ(plan->erase_regions()[index].start, blocks_for(diesel)[index].start);
                    EXPECT_EQ(plan->erase_regions()[index].length, blocks_for(diesel)[index].length);
                }
            }
            EXPECT_TRUE(validate_subaru_denso_sh7058_can_diesel_plan(*plan).has_value());
        }
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, RejectsNearMissesAndWrongMcus)
{
    for (const std::string_view protocol :
         {"unknown_diesel_can", "sub_ecu_denso_sh7058_can_diesel_future", "sub_ecu_denso_sh7059_can_diesel_extra",
          "sub_ecu_denso_sh7058_can", "sub_ecu_denso_sh7058_densocan", "sub_tcu_denso_sh7058_can"})
    {
        const auto plan = build_subaru_denso_sh7058_can_diesel_plan(FlashOperation::Read, protocol, "SH7058d",
                                                                    std::nullopt, kernel_for(kDiesel.front()));
        ASSERT_FALSE(plan.has_value()) << protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
    for (const DieselCase& diesel : kDiesel)
    {
        const auto plan = build_subaru_denso_sh7058_can_diesel_plan(FlashOperation::Read, diesel.protocol,
                                                                    diesel.mcu == "SH7058d" ? "SH7059d" : "SH7058d",
                                                                    std::nullopt, kernel_for(diesel));
        ASSERT_FALSE(plan.has_value()) << diesel.protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, EnforcesImageAndKernelBoundsPerGeneration)
{
    for (const DieselCase& diesel : kDiesel)
    {
        const auto read_with_image = build_subaru_denso_sh7058_can_diesel_plan(
            FlashOperation::Read, diesel.protocol, diesel.mcu, bytes::Bytes(diesel.rom_size, 0), kernel_for(diesel));
        ASSERT_FALSE(read_with_image.has_value());
        EXPECT_EQ(read_with_image.error().kind, ErrorKind::InvalidConfig);

        for (const FlashOperation operation : {FlashOperation::TestWrite, FlashOperation::Write})
        {
            for (const std::optional<bytes::Bytes>& image :
                 {std::optional<bytes::Bytes>{}, std::optional<bytes::Bytes>{bytes::Bytes(diesel.rom_size - 1, 0)},
                  std::optional<bytes::Bytes>{bytes::Bytes(diesel.rom_size + 1, 0)}})
            {
                const auto plan = build_subaru_denso_sh7058_can_diesel_plan(operation, diesel.protocol, diesel.mcu,
                                                                            image, kernel_for(diesel));
                ASSERT_FALSE(plan.has_value()) << diesel.protocol;
                EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
            }
        }

        const auto exact = build_subaru_denso_sh7058_can_diesel_plan(
            FlashOperation::Read, diesel.protocol, diesel.mcu, std::nullopt,
            kernel_for(diesel, bytes::Bytes(diesel.kernel_region_size, bytes::Byte{0xA5})));
        if (diesel.mcu == "SH7058d")
        {
            ASSERT_TRUE(exact.has_value()) << exact.error().detail;
        }
        else
        {
            ASSERT_FALSE(exact.has_value());
            EXPECT_EQ(exact.error().kind, ErrorKind::InvalidConfig);
        }

        for (KernelImage invalid : {
                 KernelImage{.id = "wrong-address", .load_address = diesel.kernel_address + 1, .bytes = {0x01}},
                 KernelImage{.id = "empty", .load_address = diesel.kernel_address, .bytes = {}},
             })
        {
            const auto plan = build_subaru_denso_sh7058_can_diesel_plan(FlashOperation::Read, diesel.protocol,
                                                                        diesel.mcu, std::nullopt, std::move(invalid));
            ASSERT_FALSE(plan.has_value()) << diesel.protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::InvalidConfig);
        }
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, ValidatorRejectsWireGeometryTransportAndConfirmationDrift)
{
    for (const DieselCase& diesel : kDiesel)
    {
        for (int mutation = 0; mutation < 7; ++mutation)
        {
            auto fields = valid_fields(diesel);
            auto& wire = std::get<SubaruDensoSh7058CanDieselPlan>(fields.family_plan);
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
                fields.transfer_region.length -= 1;
                break;
            case 5:
                fields.erase_regions.pop_back();
                break;
            case 6:
                fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::CycleIgnition}};
                break;
            default:
                FAIL() << "unexpected validator mutation " << mutation;
            }
            auto plan = validate_and_build(std::move(fields));
            ASSERT_TRUE(plan.has_value()) << plan.error().detail;
            const auto valid = validate_subaru_denso_sh7058_can_diesel_plan(*plan);
            ASSERT_FALSE(valid.has_value()) << diesel.protocol << " mutation " << mutation;
            EXPECT_EQ(valid.error().kind, ErrorKind::InvalidConfig);
        }
    }
}

} // namespace
} // namespace fastecu::flash
