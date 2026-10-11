#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

struct Variant
{
    std::string_view protocol;
    std::string_view mcu;
    std::uint32_t rom_size;
    bool writable;
};

constexpr auto kVariants = std::to_array<Variant>({
    {"sub_ecu_unisia_jecs_20", "M32R_128KB", 0x20000, true},
    {"sub_ecu_unisia_jecs_30", "M32R_256KB", 0x40000, true},
    {"sub_ecu_unisia_jecs_40", "M32R_384KB", 0x60000, false},
    {"sub_ecu_unisia_jecs_70", "M32R_512KB", 0x80000, false},
});

constexpr SubaruUnisiaJecsM32rKlinePlan kWire{.initial_baud = 4800, .tester_id = 0xf0, .target_id = 0x10};

FlashPlanFields ReadFields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kSubaruUnisiaJecsM32rKline,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_unisia_jecs_20",
        .mcu_name = "M32R_128KB",
        .transfer_region = MemoryRegion{0x100000, 0x20000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = kWire,
        .confirmations = {},
    };
}

FlashPlanFields WriteFields()
{
    FlashPlanFields fields = ReadFields();
    fields.operation = FlashOperation::kWrite;
    fields.transfer_region = MemoryRegion{0, 0x20000};
    fields.image = bytes::Bytes(0x20000, 0xab);
    return fields;
}

TEST(SubaruUnisiaJecsM32rKlinePlan, ReadCoversEachVariantFromTheReadBase)
{
    for (const Variant& variant : kVariants)
    {
        const auto plan = BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kRead, variant.protocol, variant.mcu,
                                                             std::nullopt, false);
        ASSERT_THAT(plan, IsOk()) << variant.protocol;
        EXPECT_EQ(plan->Family(), FlashFamily::kSubaruUnisiaJecsM32rKline);
        EXPECT_EQ(plan->Transport(), TransportKind::kKline);
        EXPECT_EQ(plan->TransferRegion(), (MemoryRegion{0x100000, variant.rom_size})) << variant.protocol;
        EXPECT_FALSE(plan->Image().has_value());
        EXPECT_FALSE(plan->Kernel().has_value());
        EXPECT_TRUE(plan->Confirmations().empty()) << "Read never raises programming voltage";
        const auto& wire = std::get<SubaruUnisiaJecsM32rKlinePlan>(plan->FamilyPlan());
        EXPECT_EQ(wire.initial_baud, 4800);
        EXPECT_EQ(wire.tester_id, 0xf0);
        EXPECT_EQ(wire.target_id, 0x10);
    }
}

TEST(SubaruUnisiaJecsM32rKlinePlan, WriteCarriesTheImageAndAsksForVppOnlyWithoutAdapterSupply)
{
    for (const Variant& variant : kVariants)
    {
        if (!variant.writable)
        {
            continue;
        }
        const auto prompted = BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kWrite, variant.protocol, variant.mcu,
                                                                 bytes::Bytes(variant.rom_size, 0x5a), false);
        ASSERT_THAT(prompted, IsOk()) << variant.protocol;
        EXPECT_EQ(prompted->TransferRegion(), (MemoryRegion{0, variant.rom_size}));
        EXPECT_EQ(prompted->Image(), std::optional<bytes::Bytes>(bytes::Bytes(variant.rom_size, 0x5a)));
        ASSERT_EQ(prompted->Confirmations().size(), 1U);
        EXPECT_EQ(prompted->Confirmations()[0].id, ConfirmationSpec::Id::kApplyProgrammingVoltage);

        const auto supplied = BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kWrite, variant.protocol, variant.mcu,
                                                                 bytes::Bytes(variant.rom_size, 0x5a), true);
        ASSERT_THAT(supplied, IsOk()) << variant.protocol;
        EXPECT_TRUE(supplied->Confirmations().empty());
    }
}

TEST(SubaruUnisiaJecsM32rKlinePlan, RejectsEveryOtherIdentity)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_unisia_jecs_20", "M32R_256KB"},
             {"sub_ecu_unisia_jecs_70", "M32R_512KB_1block"},
             {"sub_ecu_unisia_jecs_20_bootmode", "M32R_256KB"},
             {"sub_ecu_unisia_jecs_40_bootmode", "M32R_384KB"},
             {"sub_ecu_unisia_jecs_30x", "M32R_256KB"},
             {"sub_ecu_unisia_jecs_m3779x", "M3779x"},
         }))
    {
        EXPECT_THAT(BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kRead, protocol, mcu, std::nullopt, false),
                    IsErr(ErrorKind::kInvalidConfig))
            << protocol << " / " << mcu;
    }
}

// Bootmode Read is byte-identical to this family's Read
// (flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp:112-275); bootmode
// Write belongs to the bootmode family.
TEST(SubaruUnisiaJecsM32rKlinePlan, AcceptsBootmodeProtocolsForReadOnly)
{
    struct Bootmode
    {
        std::string_view protocol;
        std::string_view mcu;
        std::uint32_t rom_size;
    };
    for (const Bootmode& variant : std::to_array<Bootmode>({
             {"sub_ecu_unisia_jecs_20_bootmode", "M32R_128KB", 0x20000},
             {"sub_ecu_unisia_jecs_30_bootmode", "M32R_256KB", 0x40000},
         }))
    {
        const auto read = BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kRead, variant.protocol, variant.mcu,
                                                             std::nullopt, false);
        ASSERT_THAT(read, IsOk()) << variant.protocol;
        EXPECT_EQ(read->TransferRegion(), (MemoryRegion{0x100000, variant.rom_size}));
        EXPECT_TRUE(read->Confirmations().empty());
        EXPECT_THAT(ValidateSubaruUnisiaJecsM32rKlinePlan(*read), IsOk());

        for (const FlashOperation operation : {FlashOperation::kWrite, FlashOperation::kTestWrite})
        {
            EXPECT_THAT(BuildSubaruUnisiaJecsM32rKlinePlan(operation, variant.protocol, variant.mcu,
                                                           bytes::Bytes(variant.rom_size, 0x00), false),
                        IsErr(ErrorKind::kUnsupported))
                << variant.protocol;
        }
    }
}

TEST(SubaruUnisiaJecsM32rKlinePlan, RejectsTestWriteEverywhereAndWriteOnReadOnlyVariants)
{
    for (const Variant& variant : kVariants)
    {
        EXPECT_THAT(BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kTestWrite, variant.protocol, variant.mcu,
                                                       bytes::Bytes(variant.rom_size, 0x00), false),
                    IsErr(ErrorKind::kUnsupported))
            << variant.protocol;
        if (!variant.writable)
        {
            EXPECT_THAT(BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kWrite, variant.protocol, variant.mcu,
                                                           bytes::Bytes(variant.rom_size, 0x00), false),
                        IsErr(ErrorKind::kUnsupported))
                << variant.protocol;
        }
    }
}

TEST(SubaruUnisiaJecsM32rKlinePlan, WriteImageMustBeExactlyTheRomSize)
{
    constexpr std::string_view kProtocol = "sub_ecu_unisia_jecs_20";
    constexpr std::string_view kMcu = "M32R_128KB";
    // Legacy write_mem() :524 computed blocks = size / 0x80 and silently
    // dropped any tail; one byte over is exactly that trailing partial block.
    for (const std::size_t size : {std::size_t{0x20000 - 1}, std::size_t{0x20000 + 1}, std::size_t{0x10000}})
    {
        EXPECT_THAT(BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kWrite, kProtocol, kMcu,
                                                       bytes::Bytes(size, 0x00), false),
                    IsErr(ErrorKind::kInvalidConfig))
            << size;
    }
    EXPECT_THAT(BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kWrite, kProtocol, kMcu, std::nullopt, false),
                IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(
        BuildSubaruUnisiaJecsM32rKlinePlan(FlashOperation::kRead, kProtocol, kMcu, bytes::Bytes(0x20000, 0x00), false),
        IsErr(ErrorKind::kInvalidConfig));
}

TEST(SubaruUnisiaJecsM32rKlinePlan, ValidatorRejectsHandBuiltShapes)
{
    struct Case
    {
        const char *name;
        FlashPlanFields fields;
        ErrorKind expected;
    };
    std::vector<Case> cases;
    {
        auto fields = ReadFields();
        fields.family_plan = SubaruUnisiaJecsM32rKlinePlan{.initial_baud = 9600, .tester_id = 0xf0, .target_id = 0x10};
        cases.push_back({"wrong baud", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = ReadFields();
        fields.family_plan = SubaruUnisiaJecsM32rKlinePlan{.initial_baud = 4800, .tester_id = 0xf1, .target_id = 0x10};
        cases.push_back({"wrong tester id", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = ReadFields();
        fields.transfer_region = MemoryRegion{0, 0x20000};
        cases.push_back({"read region at flash address", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = ReadFields();
        fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::kApplyProgrammingVoltage, {}}};
        cases.push_back({"read with VPP confirmation", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = ReadFields();
        fields.kernel = KernelImage{.id = "k", .load_address = 0, .bytes = bytes::Bytes{1}};
        cases.push_back({"kernel", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields();
        fields.transfer_region = MemoryRegion{0x100000, 0x20000};
        cases.push_back({"write region at read address", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields();
        fields.image = bytes::Bytes(0x1ff80, 0xab);
        cases.push_back({"short image", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields();
        fields.erase_regions = {MemoryRegion{0, 0x20000}};
        cases.push_back({"erase regions", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields();
        fields.confirmations = {ConfirmationSpec{ConfirmationSpec::Id::kEraseTrigger, {}}};
        cases.push_back({"foreign confirmation", std::move(fields), ErrorKind::kInvalidConfig});
    }
    {
        auto fields = WriteFields();
        fields.operation = FlashOperation::kTestWrite;
        cases.push_back({"test write", std::move(fields), ErrorKind::kUnsupported});
    }
    {
        auto fields = WriteFields();
        fields.target_id = "sub_ecu_unisia_jecs_40";
        fields.mcu_name = "M32R_384KB";
        fields.transfer_region = MemoryRegion{0, 0x60000};
        fields.image = bytes::Bytes(0x60000, 0xab);
        cases.push_back({"write on read-only variant", std::move(fields), ErrorKind::kUnsupported});
    }

    for (auto& test_case : cases)
    {
        auto plan = ValidateAndBuild(std::move(test_case.fields));
        ASSERT_THAT(plan, IsOk()) << test_case.name;
        EXPECT_THAT(ValidateSubaruUnisiaJecsM32rKlinePlan(*plan), IsErr(test_case.expected)) << test_case.name;
    }
}
} // namespace
} // namespace fastecu::flash
