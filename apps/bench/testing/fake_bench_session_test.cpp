#include "apps/bench/testing/fake_bench_session.h"

#include <gtest/gtest.h>

namespace fastecu::bench::testing
{
namespace
{

TEST(FakeBenchSession, RecordsRequestsAndDequeuesRepliesInOrder)
{
    FakeBenchSession session;
    session.replies = {bytes::Bytes{0x63, 0x00}, bytes::Bytes{0x71, 0xE0, 0x00}};

    const bytes::Bytes first{0x23, 0x00, 0x02, 0x00, 0x01};
    EXPECT_EQ(session.exchange(first, uds::ExchangePolicy{}).value(), (bytes::Bytes{0x63, 0x00}));
    EXPECT_EQ(session.exchange_raw(bytes::Bytes{0x31, 0xE0}, 500).value(), (bytes::Bytes{0x71, 0xE0, 0x00}));
    ASSERT_EQ(session.requests.size(), 2u);
    EXPECT_EQ(session.requests[0], first);
}

TEST(FakeBenchSession, FailsLoudlyWhenTheScriptRunsOut)
{
    FakeBenchSession session;
    const auto result = session.exchange(bytes::Bytes{0x31, 0xE0}, uds::ExchangePolicy{});

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
}

} // namespace
} // namespace fastecu::bench::testing
