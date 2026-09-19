#include "src/backend/ports/testing/result_matchers.h"
// single_window_plan_test.cpp
#include "src/backend/flash/ecu/single_window_plan.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/ecu/subaru_denso_sh72531_can_types.h"

namespace
{
using fastecu::ErrorKind;
using fastecu::flash::build_single_window_plan;
using fastecu::flash::FlashFamily;
using fastecu::flash::FlashOperation;
using fastecu::flash::MemoryRegion;
using fastecu::flash::SingleWindowPlanSpec;
using fastecu::flash::SubaruDensoSh72531CanPlan;
using fastecu::flash::TransportKind;
using fastecu::flash::validate_single_window_plan;
using testing::HasSubstr;
using testing::IsEmpty;

constexpr std::array kProtocols{std::string_view{"sub_ecu_denso_sh72531_can"}};
constexpr MemoryRegion kBlock{0x00008000, 0x00137F00};

bool geometry_ok(const flashdev_t& device)
{
    return device.numblocks == 3;
}

bool wire_params_ok(const fastecu::flash::FlashPlan& plan)
{
    const auto *p = std::get_if<SubaruDensoSh72531CanPlan>(&plan.family_plan());
    return p != nullptr && p->request_id == 0x7e0U;
}

constexpr SingleWindowPlanSpec kSpec{
    .display_name = "Test Single Window Family",
    .protocols = kProtocols,
    .mcu = "SH72531",
    .family = FlashFamily::SubaruDensoSh72531Can,
    .transport = TransportKind::CanIso15765,
    .read_region = kBlock,
    .write_region = kBlock,
    .image_size = 0x140000,
    .geometry_ok = geometry_ok,
    .wire_params_ok = wire_params_ok,
};

fastecu::flash::FamilyPlan wire()
{
    return SubaruDensoSh72531CanPlan{0x7e0, 0x7e8, 500000, false, 0x8000, 0x100};
}

TEST(SingleWindowPlan, ReadPlanCarriesReadRegionAndNoErase)
{
    auto plan = build_single_window_plan(kSpec, FlashOperation::Read, "sub_ecu_denso_sh72531_can", "SH72531",
                                         std::nullopt, wire());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->transfer_region().start, 0x00008000U);
    EXPECT_EQ(plan->transfer_region().length, 0x00137F00U);
    EXPECT_THAT(plan->erase_regions(), IsEmpty());
    EXPECT_FALSE(plan->kernel().has_value());
}

TEST(SingleWindowPlan, WritePlanCarriesImageAndOneEraseRegion)
{
    auto plan = build_single_window_plan(kSpec, FlashOperation::Write, "sub_ecu_denso_sh72531_can", "SH72531",
                                         bytes::Bytes(0x140000, 0x00), wire());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ASSERT_EQ(plan->erase_regions().size(), 1U);
    EXPECT_EQ(plan->erase_regions()[0].start, 0x00008000U);
    EXPECT_EQ(plan->erase_regions()[0].length, 0x00137F00U);
    ASSERT_TRUE(plan->image().has_value());
    EXPECT_EQ(plan->image()->size(), 0x140000U);
}

TEST(SingleWindowPlan, UnknownProtocolIsRejectedWithTheDisplayName)
{
    auto plan =
        build_single_window_plan(kSpec, FlashOperation::Read, "not_a_protocol", "SH72531", std::nullopt, wire());
    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(plan.error().detail, HasSubstr("Test Single Window Family"));
}

TEST(SingleWindowPlan, UnknownMcuIsRejected)
{
    auto plan = build_single_window_plan(kSpec, FlashOperation::Read, "sub_ecu_denso_sh72531_can", "NOT_AN_MCU",
                                         std::nullopt, wire());
    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(plan.error().detail, HasSubstr("Unknown MCU type"));
}

TEST(SingleWindowPlan, KnownButWrongMcuIsRejected)
{
    auto plan = build_single_window_plan(kSpec, FlashOperation::Read, "sub_ecu_denso_sh72531_can", "N83M_1_5MB",
                                         std::nullopt, wire());
    ASSERT_THAT(plan, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(plan.error().detail, HasSubstr("expects MCU"));
}

TEST(SingleWindowPlan, FailingGeometryPredicateIsReportedAgainstTheMcuName)
{
    static constexpr SingleWindowPlanSpec kBadGeometry{
        .display_name = "Test Single Window Family",
        .protocols = kProtocols,
        .mcu = "SH72531",
        .family = FlashFamily::SubaruDensoSh72531Can,
        .transport = TransportKind::CanIso15765,
        .read_region = kBlock,
        .write_region = kBlock,
        .image_size = 0x140000,
        .geometry_ok = [](const flashdev_t&) { return false; },
        .wire_params_ok = wire_params_ok,
    };
    auto plan = build_single_window_plan(kBadGeometry, FlashOperation::Read, "sub_ecu_denso_sh72531_can", "SH72531",
                                         std::nullopt, wire());
    ASSERT_THAT(plan, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(plan.error().detail, HasSubstr("SH72531 flash geometry is invalid"));
}

TEST(SingleWindowPlan, TestWriteIsRejectedAsUnsupported)
{
    ASSERT_THAT(build_single_window_plan(kSpec, FlashOperation::TestWrite, "sub_ecu_denso_sh72531_can", "SH72531",
                                         std::nullopt, wire()),
                fastecu::testing::IsErr(ErrorKind::Unsupported));
}

TEST(SingleWindowPlan, WriteWithNoImageIsRejected)
{
    ASSERT_THAT(build_single_window_plan(kSpec, FlashOperation::Write, "sub_ecu_denso_sh72531_can", "SH72531",
                                         std::nullopt, wire()),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(SingleWindowPlan, WriteWithWrongImageSizeReportsUppercaseHexAndTheActualSize)
{
    auto plan = build_single_window_plan(kSpec, FlashOperation::Write, "sub_ecu_denso_sh72531_can", "SH72531",
                                         bytes::Bytes(0x10, 0x00), wire());
    ASSERT_THAT(plan, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(plan.error().detail, HasSubstr("0x140000"));
    EXPECT_THAT(plan.error().detail, HasSubstr("got 0x10 bytes"));
}

TEST(SingleWindowPlan, WrongWireParametersAreRejected)
{
    auto plan =
        build_single_window_plan(kSpec, FlashOperation::Read, "sub_ecu_denso_sh72531_can", "SH72531", std::nullopt,
                                 SubaruDensoSh72531CanPlan{0x123, 0x7e8, 500000, false, 0x8000, 0x100});
    ASSERT_THAT(plan, ::testing::Not(fastecu::testing::IsOk()));
    EXPECT_THAT(plan.error().detail, HasSubstr("wire parameters are invalid"));
}

TEST(SingleWindowPlan, ValidateAcceptsAPlanTheBuilderProduced)
{
    auto plan = build_single_window_plan(kSpec, FlashOperation::Read, "sub_ecu_denso_sh72531_can", "SH72531",
                                         std::nullopt, wire());
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(validate_single_window_plan(kSpec, *plan), fastecu::testing::IsOk());
}

TEST(SingleWindowPlan, SupportsWriteDefaultsToTrueAndGatesWriteWhenFalse)
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
        .family = FlashFamily::SubaruDensoSh72531Can,
        .transport = TransportKind::CanIso15765,
        .read_region = kRead,
        .write_region = kWrite,
        .image_size = 0x180000,
        .geometry_ok = geometry_ok,
        .wire_params_ok = wire_params_ok,
    };
    auto read = build_single_window_plan(kSplit, FlashOperation::Read, "sub_ecu_denso_sh72531_can", "SH72531",
                                         std::nullopt, wire());
    ASSERT_THAT(read, fastecu::testing::IsOk());
    EXPECT_EQ(read->transfer_region().start, 0x8000U);

    auto write = build_single_window_plan(kSplit, FlashOperation::Write, "sub_ecu_denso_sh72531_can", "SH72531",
                                          bytes::Bytes(0x180000, 0x00), wire());
    ASSERT_THAT(write, fastecu::testing::IsOk());
    EXPECT_EQ(write->transfer_region().start, 0x80000U);
    ASSERT_EQ(write->erase_regions().size(), 1U);
    EXPECT_EQ(write->erase_regions()[0].start, 0x80000U);
}
} // namespace
