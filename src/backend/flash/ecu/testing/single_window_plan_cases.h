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
    // False for families whose SingleWindowPlanSpec::supports_write is false:
    // every Write is rejected as Unsupported, so the write half of this
    // contract expects that instead of the usual InvalidConfig outcomes.
    bool supports_write = true;
};

inline std::string CaseName(const ::testing::TestParamInfo<SingleWindowPlanCase>& info)
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

    const auto plan = c.build(FlashOperation::kRead, c.protocol, c.mcu, std::nullopt);

    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_THAT(plan->TransferRegion(), RegionIs(c.read_region));
    EXPECT_THAT(plan->EraseRegions(), ::testing::IsEmpty());
    EXPECT_FALSE(plan->Kernel().has_value());
    EXPECT_THAT(plan->Confirmations(), ::testing::IsEmpty());
}

TEST_P(SingleWindowPlanContract, TestWriteIsRejectedBeforeAnyIo)
{
    const SingleWindowPlanCase& c = GetParam();

    const auto plan = c.build(FlashOperation::kTestWrite, c.protocol, c.mcu, std::nullopt);

    EXPECT_THAT(plan, fastecu::testing::IsErr(ErrorKind::kUnsupported));
}

TEST_P(SingleWindowPlanContract, WriteWithNoImageIsRejected)
{
    const SingleWindowPlanCase& c = GetParam();

    const auto plan = c.build(FlashOperation::kWrite, c.protocol, c.mcu, std::nullopt);

    EXPECT_THAT(plan, fastecu::testing::IsErr(c.supports_write ? ErrorKind::kInvalidConfig : ErrorKind::kUnsupported));
}

TEST_P(SingleWindowPlanContract, WriteRequiresAFullStartAlignedImage)
{
    const SingleWindowPlanCase& c = GetParam();

    const auto too_short = c.build(FlashOperation::kWrite, c.protocol, c.mcu, bytes::Bytes(c.image_size - 1, 0x00));

    EXPECT_THAT(too_short,
                fastecu::testing::IsErr(c.supports_write ? ErrorKind::kInvalidConfig : ErrorKind::kUnsupported));

    const auto ok = c.build(FlashOperation::kWrite, c.protocol, c.mcu, bytes::Bytes(c.image_size, 0x00));

    if (!c.supports_write)
    {
        EXPECT_THAT(ok, fastecu::testing::IsErr(ErrorKind::kUnsupported));
        return;
    }

    ASSERT_THAT(ok, fastecu::testing::IsOk());
    EXPECT_THAT(ok->EraseRegions(), ::testing::ElementsAre(RegionIs(c.erase_region)));
}

TEST_P(SingleWindowPlanContract, AForeignProtocolOrMcuIsRejected)
{
    const SingleWindowPlanCase& c = GetParam();

    EXPECT_THAT(c.build(FlashOperation::kRead, c.foreign_protocol, c.mcu, std::nullopt),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(c.build(FlashOperation::kRead, c.protocol, c.foreign_mcu, std::nullopt),
                fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
}

} // namespace fastecu::flash::testing
