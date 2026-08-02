#include <gtest/gtest.h>

#include "src/algorithms/protocol/testing/byte_test_utils.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/error.h"
#include "src/backend/protocol/testing/scripted_can_transport.h"
#include "src/backend/protocol/testing/scripted_kline_transport.h"
#include "src/backend/protocol/testing/scripted_ssm_transport.h"

using namespace mutdma;

TEST(TransportContract, NoFrameIsSuccessfulEmptyOptional)
{
    ScriptedSsmTransport t;
    t.queue_no_frame();
    fastecu::FakeCancellationToken token;
    auto result = t.read(20, token);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->has_value());
}

TEST(TransportContract, CancellationIsNotSilence)
{
    ScriptedSsmTransport t;
    t.queue_error(fastecu::ErrorKind::Cancelled);
    fastecu::FakeCancellationToken token(true);
    auto result = t.read(20, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
}

TEST(TransportContract, QueuedErrorsRemainDistinctFromNoFrame)
{
    ScriptedSsmTransport t;
    t.queue_error(fastecu::ErrorKind::Disconnected);
    fastecu::FakeCancellationToken token;
    auto result = t.read(20, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
}

TEST(TransportContract, CanReadReturnsFrameWithIdAndPayload)
{
    cdbg::ScriptedCanTransport t;
    t.queueRead(0x7E8, test_bytes::bytesFromHex("0102"));
    fastecu::FakeCancellationToken token;
    auto result = t.read(20, token);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().id, 0x7E8u);
    EXPECT_EQ(result->value().payload, test_bytes::bytesFromHex("0102"));
}

TEST(TestTransport, scripted_write_then_read)
{
    ScriptedKlineTransport t;
    t.expectWrite(test_bytes::bytesFromHex("A0"));
    t.queueRead(test_bytes::bytesFromHex("A5"));
    fastecu::FakeCancellationToken token;
    ASSERT_TRUE(t.setBaud(125000));
    const auto written = t.write(test_bytes::bytesFromHex("A0"));
    ASSERT_TRUE(written);
    ASSERT_EQ(*written, 1u);
    const auto read = t.read(50, token);
    ASSERT_TRUE(read);
    ASSERT_TRUE(read->has_value());
    ASSERT_EQ(read->value(), test_bytes::bytesFromHex("A5"));
    ASSERT_TRUE(t.scriptConsumed());
}

TEST(TestTransport, scripted_unexpected_write_flags)
{
    ScriptedKlineTransport t;
    t.expectWrite(test_bytes::bytesFromHex("A0"));
    const auto written = t.write(test_bytes::bytesFromHex("BB"));
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().kind, fastecu::ErrorKind::Internal);
    ASSERT_FALSE(t.ok()); // mismatch recorded
}
