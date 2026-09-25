#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
#include "src/backend/flash/flash_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash
{
namespace
{
using fastecu::testing::IsErr;
using fastecu::testing::IsOk;

constexpr std::string_view kProtocol = "sub_ecu_denso_mc68hc16y5_02_bdm";
constexpr std::string_view kMcu = "MC68HC16Y5";

KernelImage kernel(bytes::Bytes content, std::uint32_t load_address = 0x20000)
{
    return KernelImage{.id = "bdm-kernel", .load_address = load_address, .bytes = std::move(content)};
}

FlashPlanFields read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::SubaruDensoMc68hc16y5_02Bdm,
        .transport = TransportKind::Kline,
        .target_id = std::string(kProtocol),
        .mcu_name = std::string(kMcu),
        .transfer_region = MemoryRegion{0, 0x30000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = std::nullopt,
        .family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = 115200},
        .confirmations = {},
    };
}

FlashPlanFields write_fields(std::uint32_t size)
{
    FlashPlanFields fields = read_fields();
    fields.operation = FlashOperation::Write;
    fields.transfer_region = MemoryRegion{0x20000, size};
    fields.image = bytes::Bytes(size, 0xab);
    return fields;
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ReadCoversTheAddressSpaceImage)
{
    const auto plan =
        build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt, std::nullopt);
    ASSERT_THAT(plan, IsOk());
    EXPECT_EQ(plan->family(), FlashFamily::SubaruDensoMc68hc16y5_02Bdm);
    EXPECT_EQ(plan->transport(), TransportKind::Kline);
    EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0, 0x30000}));
    EXPECT_FALSE(plan->image().has_value());
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_EQ(std::get<SubaruDensoMc68hc16y5_02BdmPlan>(plan->family_plan()).baud, 115200);
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WritePadsTheKernelAndCarriesItAsTheImage)
{
    const auto plan = build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                                kernel(bytes::Bytes(33, 0xab)));
    ASSERT_THAT(plan, IsOk());
    ASSERT_TRUE(plan->image().has_value());
    const bytes::Bytes& image = *plan->image();
    ASSERT_EQ(image.size(), 64U);
    EXPECT_TRUE(std::all_of(image.begin(), image.begin() + 33, [](bytes::Byte value) { return value == 0xab; }));
    EXPECT_TRUE(std::all_of(image.begin() + 33, image.end(), [](bytes::Byte value) { return value == 0x00; }));
    EXPECT_EQ(plan->transfer_region(), (MemoryRegion{0x20000, 64}));
    EXPECT_FALSE(plan->kernel().has_value());
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WriteAcceptsAKernelThatFillsTheRamBlockExactly)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x8000, 0x01))),
                IsOk());
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x8001, 0x01))),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsEveryOtherIdentity)
{
    for (const auto& [protocol, mcu] : std::to_array<std::pair<std::string_view, std::string_view>>({
             {"sub_ecu_denso_mc68hc16y5_02_bdm", "MC68HC16Y5_TPU"},
             {"sub_ecu_denso_mc68hc16y5_02", "MC68HC16Y5"},
             {"sub_ecu_denso_mc68hc16y5_02_tpu", "MC68HC16Y5_TPU"},
             {"sub_ecu_denso_mc68hc16y5_02_bdm_x", "MC68HC16Y5"},
         }))
    {
        EXPECT_THAT(
            build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, protocol, mcu, std::nullopt, std::nullopt),
            IsErr(ErrorKind::InvalidConfig))
            << protocol << " / " << mcu;
    }
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsTestWrite)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::TestWrite, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::Unsupported));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, RejectsARomImageForEveryOperation)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu,
                                                          bytes::Bytes(0x30000, 0x00), std::nullopt),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu,
                                                          bytes::Bytes(0x30000, 0x00),
                                                          kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ReadRejectsAKernel)
{
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Read, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x20, 0x01))),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, WriteRejectsAMissingEmptyOrMisplacedKernel)
{
    EXPECT_THAT(
        build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt, std::nullopt),
        IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes{})),
                IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(build_subaru_denso_mc68hc16y5_02_bdm_plan(FlashOperation::Write, kProtocol, kMcu, std::nullopt,
                                                          kernel(bytes::Bytes(0x20, 0x01), 0x21000)),
                IsErr(ErrorKind::InvalidConfig));
}

TEST(SubaruDensoMc68hc16y5_02BdmPlan, ValidatorRejectsHandBuiltShapes)
{
    struct Case
    {
        const char *name;
        FlashPlanFields fields;
        ErrorKind expected;
    };
    std::vector<Case> cases;
    {
        auto fields = read_fields();
        fields.family_plan = SubaruDensoMc68hc16y5_02BdmPlan{.baud = 9600};
        cases.push_back({"wrong baud", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.kernel = kernel(bytes::Bytes(0x20, 0x01));
        cases.push_back({"read with kernel", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = read_fields();
        fields.transfer_region = MemoryRegion{0, 0x28000};
        cases.push_back({"packed read region", std::move(fields), ErrorKind::InvalidConfig});
    }
    cases.push_back({"unaligned image", write_fields(0x21), ErrorKind::InvalidConfig});
    cases.push_back({"oversize image", write_fields(0x8020), ErrorKind::InvalidConfig});
    {
        auto fields = write_fields(0x20);
        fields.transfer_region = MemoryRegion{0x20000, 0x40};
        cases.push_back({"region/image mismatch", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields(0x20);
        fields.transfer_region = MemoryRegion{0, 0x20};
        cases.push_back({"image outside RAM", std::move(fields), ErrorKind::InvalidConfig});
    }
    {
        auto fields = write_fields(0x20);
        fields.operation = FlashOperation::TestWrite;
        cases.push_back({"test write", std::move(fields), ErrorKind::Unsupported});
    }

    for (auto& test_case : cases)
    {
        auto plan = validate_and_build(std::move(test_case.fields));
        ASSERT_THAT(plan, IsOk()) << test_case.name;
        EXPECT_THAT(validate_subaru_denso_mc68hc16y5_02_bdm_plan(*plan), IsErr(test_case.expected)) << test_case.name;
    }
}
} // namespace
} // namespace fastecu::flash
