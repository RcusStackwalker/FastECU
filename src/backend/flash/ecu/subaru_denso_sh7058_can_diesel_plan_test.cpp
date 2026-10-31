#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"

#include <array>
#include <cstddef>
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
        SCOPED_TRACE(::testing::PrintToString(diesel));
        for (const FlashOperation operation :
             {FlashOperation::kRead, FlashOperation::kTestWrite, FlashOperation::kWrite})
        {
            SCOPED_TRACE(::testing::PrintToString(operation));
            const auto plan = BuildSubaruDensoSh7058CanDieselPlan(operation, diesel.protocol, diesel.mcu,
                                                                  ImageFor(diesel, operation), KernelFor(diesel));
            ASSERT_THAT(plan, fastecu::testing::IsOk()) << diesel.protocol;
            EXPECT_EQ(plan->Family(), FlashFamily::kSubaruDensoSh7058CanDiesel);
            EXPECT_EQ(plan->Transport(), TransportKind::kCanIso15765);
            EXPECT_EQ(plan->TargetId(), diesel.protocol);
            EXPECT_EQ(plan->McuName(), diesel.mcu);
            EXPECT_EQ(plan->TransferRegion().start, 0U);
            EXPECT_EQ(plan->TransferRegion().length, diesel.rom_size);
            EXPECT_THAT(plan->Confirmations(), IsEmpty());
            ASSERT_THAT(plan->Kernel(), Optional(_));
            EXPECT_EQ(plan->Kernel()->load_address, diesel.kernel_address);
            EXPECT_EQ(plan->Kernel()->id, std::string(diesel.protocol) + "-kernel");

            const auto& wire = plan->FamilyPlan().Get<SubaruDensoSh7058CanDieselPlan>();
            EXPECT_EQ(wire.request_id, 0x7E0U);
            EXPECT_EQ(wire.response_id, 0x7E8U);
            EXPECT_EQ(wire.bitrate, 500000);
            EXPECT_FALSE(wire.extended_id);

            if (operation == FlashOperation::kRead)
            {
                EXPECT_THAT(plan->Image(), Eq(std::nullopt));
                EXPECT_THAT(plan->EraseRegions(), IsEmpty());
            }
            else
            {
                ASSERT_THAT(plan->Image(), Optional(_));
                EXPECT_EQ(plan->Image()->size(), diesel.rom_size);
                ASSERT_EQ(plan->EraseRegions().size(), BlocksFor(diesel).size());
                for (std::size_t index = 0; index < BlocksFor(diesel).size(); ++index)
                {
                    SCOPED_TRACE(::testing::PrintToString(index));
                    EXPECT_EQ(plan->EraseRegions()[index].start, BlocksFor(diesel)[index].start);
                    EXPECT_EQ(plan->EraseRegions()[index].length, BlocksFor(diesel)[index].length);
                }
            }
            EXPECT_THAT(ValidateSubaruDensoSh7058CanDieselPlan(*plan), fastecu::testing::IsOk());
        }
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, RejectsNearMissesAndWrongMcus)
{
    for (const std::string_view protocol :
         {"unknown_diesel_can", "sub_ecu_denso_sh7058_can_diesel_future", "sub_ecu_denso_sh7059_can_diesel_extra",
          "sub_ecu_denso_sh7058_can", "sub_ecu_denso_sh7058_densocan", "sub_tcu_denso_sh7058_can"})
    {
        SCOPED_TRACE(::testing::PrintToString(protocol));
        const auto plan = BuildSubaruDensoSh7058CanDieselPlan(FlashOperation::kRead, protocol, "SH7058d", std::nullopt,
                                                              KernelFor(kDiesel.front()));
        ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig)) << protocol;
    }
    for (const DieselCase& diesel : kDiesel)
    {
        SCOPED_TRACE(::testing::PrintToString(diesel));
        const auto plan = BuildSubaruDensoSh7058CanDieselPlan(FlashOperation::kRead, diesel.protocol,
                                                              diesel.mcu == "SH7058d" ? "SH7059d" : "SH7058d",
                                                              std::nullopt, KernelFor(diesel));
        EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig)) << diesel.protocol;
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, EnforcesImageAndKernelBoundsPerGeneration)
{
    for (const DieselCase& diesel : kDiesel)
    {
        SCOPED_TRACE(::testing::PrintToString(diesel));
        const auto read_with_image = BuildSubaruDensoSh7058CanDieselPlan(
            FlashOperation::kRead, diesel.protocol, diesel.mcu, bytes::Bytes(diesel.rom_size, 0), KernelFor(diesel));
        EXPECT_THAT(read_with_image, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));

        for (const FlashOperation operation : {FlashOperation::kTestWrite, FlashOperation::kWrite})
        {
            SCOPED_TRACE(::testing::PrintToString(operation));
            for (const std::optional<bytes::Bytes>& image :
                 {std::optional<bytes::Bytes>{}, std::optional<bytes::Bytes>{bytes::Bytes(diesel.rom_size - 1, 0)},
                  std::optional<bytes::Bytes>{bytes::Bytes(diesel.rom_size + 1, 0)}})
            {
                SCOPED_TRACE(::testing::PrintToString(image));
                const auto plan = BuildSubaruDensoSh7058CanDieselPlan(operation, diesel.protocol, diesel.mcu, image,
                                                                      KernelFor(diesel));
                ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig)) << diesel.protocol;
            }
        }

        const auto exact = BuildSubaruDensoSh7058CanDieselPlan(
            FlashOperation::kRead, diesel.protocol, diesel.mcu, std::nullopt,
            KernelFor(diesel, bytes::Bytes(diesel.kernel_region_size, bytes::Byte{0xA5})));
        if (diesel.mcu == "SH7058d")
        {
            ASSERT_THAT(exact, fastecu::testing::IsOk());
        }
        else
        {
            EXPECT_THAT(exact, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
        }

        for (KernelImage invalid : {
                 KernelImage{.id = "wrong-address", .load_address = diesel.kernel_address + 1, .bytes = {0x01}},
                 KernelImage{.id = "empty", .load_address = diesel.kernel_address, .bytes = {}},
             })
        {
            SCOPED_TRACE(::testing::PrintToString(invalid));
            const auto plan = BuildSubaruDensoSh7058CanDieselPlan(FlashOperation::kRead, diesel.protocol, diesel.mcu,
                                                                  std::nullopt, std::move(invalid));
            EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig)) << diesel.protocol;
        }
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, ValidatorRejectsWireGeometryTransportAndConfirmationDrift)
{
    for (const DieselCase& diesel : kDiesel)
    {
        SCOPED_TRACE(::testing::PrintToString(diesel));
        for (int mutation = 0; mutation < 7; ++mutation)
        {
            SCOPED_TRACE(::testing::PrintToString(mutation));
            auto fields = ValidFields(diesel);
            auto& wire = fields.family_plan.Get<SubaruDensoSh7058CanDieselPlan>();
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
            ASSERT_THAT(plan, fastecu::testing::IsOk());
            const auto valid = ValidateSubaruDensoSh7058CanDieselPlan(*plan);
            EXPECT_THAT(valid, fastecu::testing::IsErr(ErrorKind::kInvalidConfig))
                << diesel.protocol << " mutation " << mutation;
        }
    }
}

TEST(SubaruDensoSh7058CanDieselPlan, RejectsAlteredAddressesAndLengthsInEveryEraseBlock)
{
    for (unsigned index = 0; index < 16; ++index)
    {
        SCOPED_TRACE(::testing::PrintToString(index));
        for (bool alter_start : {false, true})
        {
            SCOPED_TRACE(::testing::PrintToString(alter_start));
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
            ASSERT_THAT(plan, fastecu::testing::IsOk());
            const auto validation = ValidateSubaruDensoSh7058CanDieselPlan(*plan);
            EXPECT_THAT(validation,
                        fastecu::testing::IsErrWith(ErrorKind::kInvalidConfig,
                                                    ::testing::StrEq("erase geometry does not match the MCU")))
                << index;
        }
    }
}

} // namespace
} // namespace fastecu::flash
