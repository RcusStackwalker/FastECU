#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/result_matchers.h"
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
    EXPECT_THAT(session.exchange(first, uds::ExchangePolicy{}).value(),
                test_bytes::BytesEq((bytes::Bytes{0x63, 0x00})));
    EXPECT_THAT(session.exchange_raw(bytes::Bytes{0x31, 0xE0}, 500).value(),
                test_bytes::BytesEq((bytes::Bytes{0x71, 0xE0, 0x00})));
    ASSERT_EQ(session.requests.size(), 2U);
    EXPECT_THAT(session.requests[0], test_bytes::BytesEq(first));
}

TEST(FakeBenchSession, FailsLoudlyWhenTheScriptRunsOut)
{
    FakeBenchSession session;
    ASSERT_THAT(session.exchange(bytes::Bytes{0x31, 0xE0}, uds::ExchangePolicy{}),
                fastecu::testing::IsErr(ErrorKind::Internal));
}

} // namespace
} // namespace fastecu::bench::testing
