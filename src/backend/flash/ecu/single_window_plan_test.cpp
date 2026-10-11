#include "src/backend/ports/testing/result_matchers.h"
// single_window_plan_test.cpp
#include "src/backend/flash/ecu/single_window_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_denso_sh72531_can_types.h"

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::BuildSingleWindowPlan;
using fastecu::flash::FlashFamily;
using fastecu::flash::FlashOperation;
using fastecu::flash::MemoryRegion;
using fastecu::flash::SingleWindowPlanSpec;
using fastecu::flash::SubaruDensoSh72531CanPlan;
using fastecu::flash::TransportKind;
using fastecu::flash::ValidateSingleWindowPlan;
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_sh72531_can"}};
constexpr MemoryRegion kBlock{0x00008000, 0x00137F00};

bool GeometryOk(const FlashDevice& device)
{
    return device.numblocks == 3;
}

bool WireParamsOk(const fastecu::flash::FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruDensoSh72531CanPlan>(&plan.FamilyPlan());
    return p != nullptr && p->request_id == 0x7e0U;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Test Single Window Family",
    .protocols = kProtocols,
    .mcu = "SH72531",
    .family = FlashFamily::kSubaruDensoSh72531Can,
    .transport = TransportKind::kCanIso15765,
    .read_region = kBlock,
    .write_region = kBlock,
    .image_size = 0x140000,
    .geometry_ok = GeometryOk,
    .wire_params_ok = WireParamsOk,
};

fastecu::flash::FamilyPlan Wire()
{
    return SubaruDensoSh72531CanPlan{0x7e0, 0x7e8, 500000, false, 0x8000, 0x100};
}

TEST(SingleWindowPlan, ReadPlanCarriesReadRegionAndNoErase)
{
    auto plan = BuildSingleWindowPlan(kSpec, FlashOperation::kRead, "sub_ecu_denso_sh72531_can", "SH72531",
                                      std::nullopt, Wire());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->TransferRegion().start, 0x00008000U);
    EXPECT_EQ(plan->TransferRegion().length, 0x00137F00U);
    EXPECT_THAT(plan->EraseRegions(), IsEmpty());
    EXPECT_FALSE(plan->Kernel().has_value());
}

TEST(SingleWindowPlan, WritePlanCarriesImageAndOneEraseRegion)
{
    auto plan = BuildSingleWindowPlan(kSpec, FlashOperation::kWrite, "sub_ecu_denso_sh72531_can", "SH72531",
                                      bytes::Bytes(0x140000, 0x00), Wire());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ASSERT_EQ(plan->EraseRegions().size(), 1U);
    EXPECT_EQ(plan->EraseRegions()[0].start, 0x00008000U);
    EXPECT_EQ(plan->EraseRegions()[0].length, 0x00137F00U);
    ASSERT_TRUE(plan->Image().has_value());
    EXPECT_EQ(plan->Image()->size(), 0x140000U);
}

TEST(SingleWindowPlan, UnknownProtocolIsRejectedWithTheDisplayName)
{
    auto plan = BuildSingleWindowPlan(kSpec, FlashOperation::kRead, "not_a_protocol", "SH72531", std::nullopt, Wire());
    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, HasSubstr("Test Single Window Family"));
}

TEST(SingleWindowPlan, UnknownMcuIsRejected)
{
    auto plan = BuildSingleWindowPlan(kSpec, FlashOperation::kRead, "sub_ecu_denso_sh72531_can", "NOT_AN_MCU",
                                      std::nullopt, Wire());
    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, HasSubstr("Unknown MCU type"));
}

TEST(SingleWindowPlan, KnownButWrongMcuIsRejected)
{
    auto plan = BuildSingleWindowPlan(kSpec, FlashOperation::kRead, "sub_ecu_denso_sh72531_can", "N83M_1_5MB",
                                      std::nullopt, Wire());
    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(plan.error().detail, HasSubstr("expects MCU"));
}

TEST(SingleWindowPlan, FailingGeometryPredicateIsReportedAgainstTheMcuName)
{
    static constexpr SingleWindowPlanSpec kBadGeometry{
        .display_name = "Test Single Window Family",
        .protocols = kProtocols,
        .mcu = "SH72531",
        .family = FlashFamily::kSubaruDensoSh72531Can,
        .transport = TransportKind::kCanIso15765,
        .read_region = kBlock,
        .write_region = kBlock,
        .image_size = 0x140000,
        .geometry_ok = [](const FlashDevice&) { return false; },
        .wire_params_ok = WireParamsOk,
    };
    auto plan = BuildSingleWindowPlan(kBadGeometry, FlashOperation::kRead, "sub_ecu_denso_sh72531_can", "SH72531",
                                      std::nullopt, Wire());
    ASSERT_THAT(plan, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(plan.error().detail, HasSubstr("SH72531 flash geometry is invalid"));
}

TEST(SingleWindowPlan, TestWriteIsRejectedAsUnsupported)
{
    ASSERT_THAT(BuildSingleWindowPlan(kSpec, FlashOperation::kTestWrite, "sub_ecu_denso_sh72531_can", "SH72531",
                                      std::nullopt, Wire()),
                fastecu::testing::IsErr(ErrorKind::kUnsupported));
}

TEST(SingleWindowPlan, WriteWithNoImageIsRejected)
{
    ASSERT_THAT(BuildSingleWindowPlan(kSpec, FlashOperation::kWrite, "sub_ecu_denso_sh72531_can", "SH72531",
                                      std::nullopt, Wire()),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

TEST(SingleWindowPlan, WriteWithWrongImageSizeReportsUppercaseHexAndTheActualSize)
{
    auto plan = BuildSingleWindowPlan(kSpec, FlashOperation::kWrite, "sub_ecu_denso_sh72531_can", "SH72531",
                                      bytes::Bytes(0x10, 0x00), Wire());
    ASSERT_THAT(plan, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(plan.error().detail, HasSubstr("0x140000"));
    EXPECT_THAT(plan.error().detail, HasSubstr("got 0x10 bytes"));
}

TEST(SingleWindowPlan, WrongWireParametersAreRejected)
{
    auto plan =
        BuildSingleWindowPlan(kSpec, FlashOperation::kRead, "sub_ecu_denso_sh72531_can", "SH72531", std::nullopt,
                              SubaruDensoSh72531CanPlan{0x123, 0x7e8, 500000, false, 0x8000, 0x100});
    ASSERT_THAT(plan, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(plan.error().detail, HasSubstr("wire parameters are invalid"));
}

TEST(SingleWindowPlan, ValidateAcceptsAPlanTheBuilderProduced)
{
    auto plan = BuildSingleWindowPlan(kSpec, FlashOperation::kRead, "sub_ecu_denso_sh72531_can", "SH72531",
                                      std::nullopt, Wire());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(ValidateSingleWindowPlan(kSpec, *plan), fastecu::testing::IsOk());
}

TEST(SingleWindowPlan, SupportsWriteDefaultsToTrue)
{
    EXPECT_TRUE(SingleWindowPlanSpec{}.supports_write);
}

TEST(SingleWindowPlan, DistinctReadAndWriteWindowsAreHonoured)
{
    static constexpr MemoryRegion kRead{0x8000, 0x78000};
    static constexpr MemoryRegion kWrite{0x80000, 0x100000};
    static constexpr SingleWindowPlanSpec kSplit{
        .display_name = "Test Split Window Family",
        .protocols = kProtocols,
        .mcu = "SH72531",
        .family = FlashFamily::kSubaruDensoSh72531Can,
        .transport = TransportKind::kCanIso15765,
        .read_region = kRead,
        .write_region = kWrite,
        .image_size = 0x180000,
        .geometry_ok = GeometryOk,
        .wire_params_ok = WireParamsOk,
    };
    auto read = BuildSingleWindowPlan(kSplit, FlashOperation::kRead, "sub_ecu_denso_sh72531_can", "SH72531",
                                      std::nullopt, Wire());
    ASSERT_THAT(read, fastecu::testing::IsOk());
    EXPECT_EQ(read->TransferRegion().start, 0x8000U);

    auto write = BuildSingleWindowPlan(kSplit, FlashOperation::kWrite, "sub_ecu_denso_sh72531_can", "SH72531",
                                       bytes::Bytes(0x180000, 0x00), Wire());
    ASSERT_THAT(write, fastecu::testing::IsOk());
    EXPECT_EQ(write->TransferRegion().start, 0x80000U);
    ASSERT_EQ(write->EraseRegions().size(), 1U);
    EXPECT_EQ(write->EraseRegions()[0].start, 0x80000U);
}
} // namespace
