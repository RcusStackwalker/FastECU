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

const std::array<MemoryRegion, 16>& BlocksFor(const DieselCase& diesel)
{
    return diesel.mcu == "SH7058d" ? kSh7058Blocks : kSh7059Blocks;
}

KernelImage KernelFor(const DieselCase& diesel, bytes::Bytes data = {0x01, 0x02, 0x03, 0x04})
{
    return {.id = std::string(diesel.protocol) + "-kernel",
            .load_address = diesel.kernel_address,
            .bytes = std::move(data)};
}

std::optional<bytes::Bytes> ImageFor(const DieselCase& diesel, FlashOperation operation)
{
    if (operation == FlashOperation::kRead)
    {
        return std::nullopt;
    }
    return bytes::Bytes(diesel.rom_size, bytes::Byte{0});
}

FlashPlanFields ValidFields(const DieselCase& diesel, FlashOperation operation = FlashOperation::kWrite)
{
    const auto& blocks = BlocksFor(diesel);
    return {
        .operation = operation,
        .family = FlashFamily::kSubaruDensoSh7058CanDiesel,
        .transport = TransportKind::kCanIso15765,
        .target_id = std::string(diesel.protocol),
        .mcu_name = std::string(diesel.mcu),
        .transfer_region = {0, static_cast<std::uint32_t>(diesel.rom_size)},
        .erase_regions = operation == FlashOperation::kRead ? std::vector<MemoryRegion>{}
                                                            : std::vector<MemoryRegion>(blocks.begin(), blocks.end()),
        .image = ImageFor(diesel, operation),
        .kernel = KernelFor(diesel),
        .family_plan = SubaruDensoSh7058CanDieselPlan{},
        .confirmations = {},
    };
}

TEST(SubaruDensoSh7058CanDieselPlan, BuildsBothExactGenerationsForEveryOperation)
{
    for (const DieselCase& diesel : kDiesel)
    {
        for (const FlashOperation operation :
             {FlashOperation::kRead, FlashOperation::kTestWrite, FlashOperation::kWrite})
        {
            const auto plan = BuildSubaruDensoSh7058CanDieselPlan(operation, diesel.protocol, diesel.mcu,
                                                                  ImageFor(diesel, operation), KernelFor(diesel));
            ASSERT_TRUE(plan.has_value()) << diesel.protocol << ": " << plan.error().detail;
            EXPECT_EQ(plan->Family(), FlashFamily::kSubaruDensoSh7058CanDiesel);
            EXPECT_EQ(plan->Transport(), TransportKind::kCanIso15765);
            EXPECT_EQ(plan->TargetId(), diesel.protocol);
            EXPECT_EQ(plan->McuName(), diesel.mcu);
            EXPECT_EQ(plan->TransferRegion().start, 0U);
            EXPECT_EQ(plan->TransferRegion().length, diesel.rom_size);
            EXPECT_TRUE(plan->Confirmations().empty());
            ASSERT_TRUE(plan->Kernel().has_value());
            EXPECT_EQ(plan->Kernel()->load_address, diesel.kernel_address);
            EXPECT_EQ(plan->Kernel()->id, std::string(diesel.protocol) + "-kernel");

            const auto& wire = std::get<SubaruDensoSh7058CanDieselPlan>(plan->FamilyPlan());
            EXPECT_EQ(wire.request_id, 0x7E0U);
            EXPECT_EQ(wire.response_id, 0x7E8U);
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
                EXPECT_EQ(plan->Image()->size(), diesel.rom_size);
                ASSERT_EQ(plan->EraseRegions().size(), BlocksFor(diesel).size());
                for (std::size_t index = 0; index < BlocksFor(diesel).size(); ++index)
                {
                    EXPECT_EQ(plan->EraseRegions()[index].start, BlocksFor(diesel)[index].start);
                    EXPECT_EQ(plan->EraseRegions()[index].length, BlocksFor(diesel)[index].length);
                }
            }
            EXPECT_TRUE(ValidateSubaruDensoSh7058CanDieselPlan(*plan).has_value());
        }
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, RejectsNearMissesAndWrongMcus)
{
    for (const std::string_view protocol :
         {"unknown_diesel_can", "sub_ecu_denso_sh7058_can_diesel_future", "sub_ecu_denso_sh7059_can_diesel_extra",
          "sub_ecu_denso_sh7058_can", "sub_ecu_denso_sh7058_densocan", "sub_tcu_denso_sh7058_can"})
    {
        const auto plan = BuildSubaruDensoSh7058CanDieselPlan(FlashOperation::kRead, protocol, "SH7058d", std::nullopt,
                                                              KernelFor(kDiesel.front()));
        ASSERT_FALSE(plan.has_value()) << protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
    }
    for (const DieselCase& diesel : kDiesel)
    {
        const auto plan = BuildSubaruDensoSh7058CanDieselPlan(FlashOperation::kRead, diesel.protocol,
                                                              diesel.mcu == "SH7058d" ? "SH7059d" : "SH7058d",
                                                              std::nullopt, KernelFor(diesel));
        ASSERT_FALSE(plan.has_value()) << diesel.protocol;
        EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, EnforcesImageAndKernelBoundsPerGeneration)
{
    for (const DieselCase& diesel : kDiesel)
    {
        const auto read_with_image = BuildSubaruDensoSh7058CanDieselPlan(
            FlashOperation::kRead, diesel.protocol, diesel.mcu, bytes::Bytes(diesel.rom_size, 0), KernelFor(diesel));
        ASSERT_FALSE(read_with_image.has_value());
        EXPECT_EQ(read_with_image.error().kind, ErrorKind::kInvalidConfig);

        for (const FlashOperation operation : {FlashOperation::kTestWrite, FlashOperation::kWrite})
        {
            for (const std::optional<bytes::Bytes>& image :
                 {std::optional<bytes::Bytes>{}, std::optional<bytes::Bytes>{bytes::Bytes(diesel.rom_size - 1, 0)},
                  std::optional<bytes::Bytes>{bytes::Bytes(diesel.rom_size + 1, 0)}})
            {
                const auto plan = BuildSubaruDensoSh7058CanDieselPlan(operation, diesel.protocol, diesel.mcu, image,
                                                                      KernelFor(diesel));
                ASSERT_FALSE(plan.has_value()) << diesel.protocol;
                EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
            }
        }

        const auto exact = BuildSubaruDensoSh7058CanDieselPlan(
            FlashOperation::kRead, diesel.protocol, diesel.mcu, std::nullopt,
            KernelFor(diesel, bytes::Bytes(diesel.kernel_region_size, bytes::Byte{0xA5})));
        if (diesel.mcu == "SH7058d")
        {
            ASSERT_TRUE(exact.has_value()) << exact.error().detail;
        }
        else
        {
            ASSERT_FALSE(exact.has_value());
            EXPECT_EQ(exact.error().kind, ErrorKind::kInvalidConfig);
        }

        for (KernelImage invalid : {
                 KernelImage{.id = "wrong-address", .load_address = diesel.kernel_address + 1, .bytes = {0x01}},
                 KernelImage{.id = "empty", .load_address = diesel.kernel_address, .bytes = {}},
             })
        {
            const auto plan = BuildSubaruDensoSh7058CanDieselPlan(FlashOperation::kRead, diesel.protocol, diesel.mcu,
                                                                  std::nullopt, std::move(invalid));
            ASSERT_FALSE(plan.has_value()) << diesel.protocol;
            EXPECT_EQ(plan.error().kind, ErrorKind::kInvalidConfig);
        }
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, ValidatorRejectsWireGeometryTransportAndConfirmationDrift)
{
    for (const DieselCase& diesel : kDiesel)
    {
        for (int mutation = 0; mutation < 7; ++mutation)
        {
            auto fields = ValidFields(diesel);
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
                fields.confirmations = {ConfirmationSpec{.id = ConfirmationSpec::Id::kCycleIgnition}};
                break;
            default:
                FAIL() << "unexpected validator mutation " << mutation;
            }
            auto plan = ValidateAndBuild(std::move(fields));
            ASSERT_TRUE(plan.has_value()) << plan.error().detail;
            const auto valid = ValidateSubaruDensoSh7058CanDieselPlan(*plan);
            ASSERT_FALSE(valid.has_value()) << diesel.protocol << " mutation " << mutation;
            EXPECT_EQ(valid.error().kind, ErrorKind::kInvalidConfig);
        }
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, RejectsAlteredAddressesAndLengthsInEveryEraseBlock)
{
    for (unsigned index = 0; index < 16; ++index)
    {
        for (bool alter_start : {false, true})
        {
            auto fields = ValidFields(kDiesel.front());
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
            const auto validation = ValidateSubaruDensoSh7058CanDieselPlan(*plan);
            ASSERT_FALSE(validation.has_value()) << index;
            EXPECT_EQ(validation.error(), (Error{ErrorKind::kInvalidConfig, "erase geometry does not match the MCU"}));
        }
    }
}

} // namespace
} // namespace fastecu::flash
