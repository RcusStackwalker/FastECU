#include <gtest/gtest.h>

#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/error.h"
#include "scripted_can_transport.h"
#include "scripted_kline_transport.h"
#include "scripted_ssm_transport.h"

using namespace mutdma;

namespace
{
class NeverCancelled final : public fastecu::ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return false;
    }
};

class Cancelled final : public fastecu::ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return true;
    }
};
} // namespace

TEST(TransportContract, NoFrameIsSuccessfulEmptyOptional)
{
    ScriptedSsmTransport t;
    t.queue_no_frame();
    NeverCancelled token;
    auto result = t.read(20, token);
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->has_value());
}

TEST(TransportContract, CancellationIsNotSilence)
{
    ScriptedSsmTransport t;
    t.queue_error(fastecu::ErrorKind::Cancelled);
    Cancelled token;
    auto result = t.read(20, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Cancelled);
}

TEST(TransportContract, QueuedErrorsRemainDistinctFromNoFrame)
{
    ScriptedSsmTransport t;
    t.queue_error(fastecu::ErrorKind::Disconnected);
    NeverCancelled token;
    auto result = t.read(20, token);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::Disconnected);
}

TEST(TransportContract, CanReadReturnsFrameWithIdAndPayload)
{
    cdbg::ScriptedCanTransport t;
    t.queueRead(0x7E8, QByteArray::fromHex("0102"));
    NeverCancelled token;
    auto result = t.read(20, token);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->has_value());
    EXPECT_EQ(result->value().id, 0x7E8u);
    EXPECT_EQ(bytes::toQByteArray(result->value().payload), QByteArray::fromHex("0102"));
}

TEST(TestTransport, scripted_write_then_read)
{
    ScriptedKlineTransport t;
    t.expectWrite(QByteArray::fromHex("A0"));
    t.queueRead(QByteArray::fromHex("A5"));
    NeverCancelled token;
    ASSERT_TRUE(t.setBaud(125000));
    const auto written = t.write(bytes::view(QByteArray::fromHex("A0")));
    ASSERT_TRUE(written);
    ASSERT_EQ(*written, 1u);
    const auto read = t.read(50, token);
    ASSERT_TRUE(read);
    ASSERT_TRUE(read->has_value());
    ASSERT_EQ(bytes::toQByteArray(read->value()), QByteArray::fromHex("A5"));
    ASSERT_TRUE(t.scriptConsumed());
}

TEST(TestTransport, scripted_unexpected_write_flags)
{
    ScriptedKlineTransport t;
    t.expectWrite(QByteArray::fromHex("A0"));
    const auto written = t.write(bytes::view(QByteArray::fromHex("BB")));
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().kind, fastecu::ErrorKind::Internal);
    ASSERT_FALSE(t.ok()); // mismatch recorded
}
