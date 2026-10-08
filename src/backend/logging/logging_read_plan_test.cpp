#include "src/backend/logging/logging_read_plan.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <gtest/gtest.h>

namespace fastecu::logging
{
TEST(LoggingReadPlanTest, ExpandsBytesInRequestOrderAndMapsLogicalPositions)
{
    const std::vector<LoggingChannel> channels{{.id = "wide", .address = 0x10, .length = 2},
                                               {.id = "single", .address = 0x20, .length = 1}};
    const auto plan = make_ssm_read_plan(channels);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->addresses, (std::vector<std::uint32_t>{0x10, 0x11, 0x20}));
    EXPECT_EQ(plan->response_positions, (std::vector<std::vector<std::size_t>>{{0, 1}, {2}}));
}
TEST(LoggingReadPlanTest, ExplicitOrderedBytesAreNotReordered)
{
    const std::vector<LoggingChannel> channels{
        {.id = "wide", .address = 0x11, .length = 2, .byte_addresses = {0x11, 0x10}}};
    const auto plan = make_ssm_read_plan(channels);
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    EXPECT_EQ(plan->addresses, (std::vector<std::uint32_t>{0x11, 0x10}));
    EXPECT_EQ(plan->response_positions, (std::vector<std::vector<std::size_t>>{{0, 1}}));
}
TEST(LoggingReadPlanTest, RejectsInconsistentOrOutOfRangeByteSources)
{
    const std::vector<LoggingChannel> short_source{
        {.id = "wide", .address = 0x10, .length = 2, .byte_addresses = {0x10}}};
    EXPECT_THAT(make_ssm_read_plan(short_source), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    const std::vector<LoggingChannel> overflow{{.id = "wide", .address = 0xffffff, .length = 2}};
    EXPECT_THAT(make_ssm_read_plan(overflow), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    const std::vector<LoggingChannel> bad_bit{{.id = "flag", .address = 0x10, .length = 1, .sample_bit = 8}};
    EXPECT_THAT(make_ssm_read_plan(bad_bit), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}
} // namespace fastecu::logging
