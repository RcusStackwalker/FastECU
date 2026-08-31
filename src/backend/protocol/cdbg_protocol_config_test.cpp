#include "src/backend/protocol/cdbg_protocol_config.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <utility>

namespace
{
TEST(CdbgProtocolConfig, PreservesConfiguredWireValues)
{
    const auto result = cdbg::make_cdbg_protocol_config(0x620, 0x621, 7, 25);

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->request_id(), 0x620U);
    EXPECT_EQ(result->reply_id(), 0x621U);
    EXPECT_EQ(result->stream_instance(), 7U);
    EXPECT_EQ(result->sampling_interval_ms(), 25U);
}

TEST(CdbgProtocolConfig, UsesColtWireDefaults)
{
    const auto result = cdbg::make_colt_cdbg_protocol_config();

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->request_id(), 0x630U);
    EXPECT_EQ(result->reply_id(), 0x631U);
    EXPECT_EQ(result->stream_instance(), 0U);
    EXPECT_EQ(result->sampling_interval_ms(), 10U);
}

TEST(CdbgProtocolConfig, RejectsEqualAndOutOfRangeIdentifiers)
{
    const std::array cases{
        std::pair{0x620U, 0x620U},
        std::pair{0x20000000U, 0x621U},
        std::pair{0x620U, 0x20000000U},
    };

    for (const auto& [request_id, reply_id] : cases)
    {
        const auto result = cdbg::make_cdbg_protocol_config(request_id, reply_id, 7, 25);
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
    }
}

TEST(CdbgProtocolConfig, AcceptsOnlyWireEncodableSamplingIntervals)
{
    struct IntervalCase
    {
        std::uint32_t value;
        bool valid;
    };
    const IntervalCase cases[] = {
        {0, false},    {1, true},      {65535, true},  {65536, false},
        {65540, true}, {65541, false}, {655350, true}, {655360, false},
    };
    for (const auto& test : cases)
    {
        auto result = cdbg::make_cdbg_protocol_config(0x620, 0x621, 7, test.value);
        EXPECT_EQ(result.has_value(), test.valid) << test.value;
        if (!test.valid)
        {
            EXPECT_EQ(result.error().kind, fastecu::ErrorKind::InvalidConfig);
        }
    }
}
} // namespace
