#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>

#include <chrono>

#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/error.h"
#include "src/backend/protocol/testing/scripted_can_transport.h"
#include "src/backend/protocol/testing/scripted_kline_transport.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

using namespace mutdma;
using namespace std::chrono_literals;

TEST(TransportContract, NoFrameIsSuccessfulEmptyOptional)
{
    ScriptedSsmTransport t;
    t.QueueNoFrame();
    fastecu::FakeCancellationToken token;
    auto result = t.Read(20ms, token);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_FALSE(result->has_value());
}

TEST(TransportContract, CancellationIsNotSilence)
{
    ScriptedSsmTransport t;
    t.QueueError(fastecu::ErrorKind::kCancelled);
    fastecu::FakeCancellationToken token(true);
    ASSERT_THAT(t.Read(20ms, token), fastecu::testing::IsErr(fastecu::ErrorKind::kCancelled));
}

TEST(TransportContract, QueuedErrorsRemainDistinctFromNoFrame)
{
    ScriptedSsmTransport t;
    t.QueueError(fastecu::ErrorKind::kDisconnected);
    fastecu::FakeCancellationToken token;
    ASSERT_THAT(t.Read(20ms, token), fastecu::testing::IsErr(fastecu::ErrorKind::kDisconnected));
}

TEST(TransportContract, CanReadReturnsFrameWithIdAndPayload)
{
    cdbg::ScriptedCanTransport t;
    t.QueueRead(0x7E8, test_bytes::BytesFromHex("0102"));
    fastecu::FakeCancellationToken token;
    auto result = t.Read(20ms, token);
    ASSERT_THAT(result, fastecu::testing::IsOk());
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().id, 0x7E8U);
    EXPECT_EQ(result->value().payload, test_bytes::BytesFromHex("0102"));
}

TEST(TestTransport, scripted_write_then_read)
{
    ScriptedKlineTransport t;
    t.ExpectWrite(test_bytes::BytesFromHex("A0"));
    t.QueueRead(test_bytes::BytesFromHex("A5"));
    fastecu::FakeCancellationToken token;
    ASSERT_THAT(t.SetBaud(125000), fastecu::testing::IsOk());
    const auto written = t.Write(test_bytes::BytesFromHex("A0"));
    ASSERT_THAT(written, fastecu::testing::IsOk());
    ASSERT_EQ(*written, 1U);
    const auto read = t.Read(50ms, token);
    ASSERT_THAT(read, fastecu::testing::IsOk());
    ASSERT_TRUE(read->has_value());
    ASSERT_EQ(read->value(), test_bytes::BytesFromHex("A5"));
    ASSERT_TRUE(t.ScriptConsumed());
}

TEST(TestTransport, scripted_unexpected_write_flags)
{
    ScriptedKlineTransport t;
    t.ExpectWrite(test_bytes::BytesFromHex("A0"));
    ASSERT_THAT(t.Write(test_bytes::BytesFromHex("BB")), fastecu::testing::IsErr(fastecu::ErrorKind::kInternal));
    ASSERT_FALSE(t.Ok()); // mismatch recorded
}
