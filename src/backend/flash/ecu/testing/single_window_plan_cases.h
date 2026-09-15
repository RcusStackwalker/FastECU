#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash::testing
{

// One family's declarative contract. The values are restated here rather than
// read from the family's own constexpr kSpec: a test that reads its
// expectations from the table under test asserts nothing.
struct SingleWindowPlanCase
{
    std::string_view name; // shows up in the gtest test name
    Result<FlashPlan> (*build)(FlashOperation, std::string_view, std::string_view, std::optional<bytes::Bytes>);
    std::string_view protocol;
    std::string_view mcu;
    std::string_view foreign_protocol; // a protocol name this family must reject
    std::string_view foreign_mcu;      // an MCU name this family must reject
    MemoryRegion read_region;
    MemoryRegion erase_region;
    std::uint32_t image_size;
};

inline std::string caseName(const ::testing::TestParamInfo<SingleWindowPlanCase>& info)
{
    return std::string(info.param.name);
}

MATCHER_P(RegionIs, expected, "")
{
    if (arg.start == expected.start && arg.length == expected.length)
    {
        return true;
    }
    *result_listener << std::format("which is [0x{:x}, len 0x{:x})", arg.start, arg.length);
    return false;
}

class SingleWindowPlanContract : public ::testing::TestWithParam<SingleWindowPlanCase>
{
};

TEST_P(SingleWindowPlanContract, ReadPlanCarriesTheTransferRegion)
{
    const SingleWindowPlanCase& c = GetParam();

    const auto plan = c.build(FlashOperation::Read, c.protocol, c.mcu, std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(plan->transfer_region(), RegionIs(c.read_region));
    EXPECT_THAT(plan->erase_regions(), ::testing::IsEmpty());
    EXPECT_FALSE(plan->kernel().has_value());
    EXPECT_THAT(plan->confirmations(), ::testing::IsEmpty());
}

TEST_P(SingleWindowPlanContract, TestWriteIsRejectedBeforeAnyIo)
{
    const SingleWindowPlanCase& c = GetParam();

    const auto plan = c.build(FlashOperation::TestWrite, c.protocol, c.mcu, std::nullopt);

    EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::Unsupported));
}

TEST_P(SingleWindowPlanContract, WriteWithNoImageIsRejected)
{
    const SingleWindowPlanCase& c = GetParam();

    const auto plan = c.build(FlashOperation::Write, c.protocol, c.mcu, std::nullopt);

    EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST_P(SingleWindowPlanContract, WriteRequiresAFullStartAlignedImage)
{
    const SingleWindowPlanCase& c = GetParam();

    const auto tooShort = c.build(FlashOperation::Write, c.protocol, c.mcu, bytes::Bytes(c.image_size - 1, 0x00));

    EXPECT_THAT(tooShort, fastecu::testing::IsErr(ErrorKind::InvalidConfig));

    const auto ok = c.build(FlashOperation::Write, c.protocol, c.mcu, bytes::Bytes(c.image_size, 0x00));

    ASSERT_THAT(ok, fastecu::testing::IsOk());
    EXPECT_THAT(ok->erase_regions(), ::testing::ElementsAre(RegionIs(c.erase_region)));
}

TEST_P(SingleWindowPlanContract, AForeignProtocolOrMcuIsRejected)
{
    const SingleWindowPlanCase& c = GetParam();

    EXPECT_THAT(c.build(FlashOperation::Read, c.foreign_protocol, c.mcu, std::nullopt),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_THAT(c.build(FlashOperation::Read, c.protocol, c.foreign_mcu, std::nullopt),
                fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

} // namespace fastecu::flash::testing
