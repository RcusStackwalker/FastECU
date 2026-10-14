#include "src/backend/flash/ecu/m32r_ssm_exchange.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <chrono>
#include <gmock/gmock-matchers.h>
#include <gtest/gtest.h>

namespace
{
using namespace fastecu;
using namespace fastecu::flash;
using namespace std::chrono_literals;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOkAnd;
using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Optional;

const bytes::Bytes kPayload{0xbf};
const bytes::Bytes kFrame{0x80, 0x10, 0xf0, 0x01, 0xbf, 0x40};

class ExchangeTransport final : public ScriptedKlineFlashTransport
{
  public:
    explicit ExchangeTransport(ManualCancellationToken& cancellation)
        : ScriptedKlineFlashTransport(ScriptedTransportInitialState::kOpen), cancellation_(cancellation)
    {
    }

    Result<std::size_t> Write(bytes::ByteView data) override
    {
        ++write_calls;
        auto result = ScriptedKlineFlashTransport::Write(data);
        if (!result.has_value())
        {
            return result;
        }
        if (cancel_on_write)
        {
            cancellation_.Cancel();
        }
        if (write_fault)
        {
            return Fail(ErrorKind::kDisconnected, "write fault");
        }
        return short_write ? data.size() - 1 : *result;
    }

    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        ++read_calls;
        auto result = ScriptedKlineFlashTransport::Read(timeout, cancellation);
        if (cancel_on_read)
        {
            cancellation_.Cancel();
        }
        return result;
    }

    int write_calls = 0;
    int read_calls = 0;
    bool short_write = false;
    bool write_fault = false;
    bool cancel_on_write = false;
    bool cancel_on_read = false;

  private:
    ManualCancellationToken& cancellation_;
};

class M32rSsmExchangeTest : public ::testing::Test
{
  protected:
    Result<std::optional<bytes::Bytes>> Exchange()
    {
        return ExchangeM32rSsmOptional(transport, cancellation, kPayload, 0xf0, 0x10, 2000ms);
    }

    ManualCancellationToken cancellation;
    ExchangeTransport transport{cancellation};
};

TEST_F(M32rSsmExchangeTest, WritesExactFrameAndUsesExplicitTimeout)
{
    transport.Exchange(kFrame, bytes::Bytes{0x01, 0x02});
    EXPECT_THAT(Exchange(), IsOkAnd(Optional(ElementsAre(0x01, 0x02))));
    EXPECT_THAT(transport.read_timeouts, ElementsAre(2000ms));
    EXPECT_EQ(transport.write_calls, 1);
    EXPECT_EQ(transport.read_calls, 1);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST_F(M32rSsmExchangeTest, PreservesNonDefaultIds)
{
    transport.Exchange(bytes::Bytes{0x80, 0x18, 0xf1, 0x01, 0xbf, 0x49}, bytes::Bytes{0x01, 0x02});
    EXPECT_THAT(ExchangeM32rSsmOptional(transport, cancellation, kPayload, 0xf1, 0x18, 500ms),
                IsOkAnd(Optional(ElementsAre(0x01, 0x02))));
    EXPECT_THAT(transport.read_timeouts, ElementsAre(500ms));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST_F(M32rSsmExchangeTest, PreservesAbsentAndPresentEmptyReplies)
{
    transport.ExpectWrite(kFrame);
    transport.QueueNoFrame();
    EXPECT_THAT(Exchange(), IsOkAnd(Eq(std::nullopt)));
    transport.Exchange(kFrame, bytes::Bytes{});
    EXPECT_THAT(Exchange(), IsOkAnd(Optional(ElementsAre())));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST_F(M32rSsmExchangeTest, PreservesLengthWrapWithoutChangingWriteSize)
{
    const bytes::Bytes payload(256, 0);
    bytes::Bytes frame{0x80, 0x10, 0xf0, 0x00};
    frame.insert(frame.end(), 256, 0);
    frame.push_back(0x80);
    ASSERT_EQ(frame.size(), 261U);
    transport.Exchange(frame, bytes::Bytes{0x01});
    EXPECT_THAT(ExchangeM32rSsmOptional(transport, cancellation, payload, 0xf0, 0x10, 2000ms),
                IsOkAnd(Optional(ElementsAre(0x01))));
    EXPECT_TRUE(transport.ScriptConsumed());
    transport.ExpectWrite(frame);
    transport.short_write = true;
    EXPECT_THAT(ExchangeM32rSsmOptional(transport, cancellation, payload, 0xf0, 0x10, 2000ms),
                IsErrWith(ErrorKind::kDisconnected, Eq("short K-Line write")));
    EXPECT_EQ(transport.read_calls, 1);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST_F(M32rSsmExchangeTest, RejectsShortWriteBeforeRead)
{
    transport.ExpectWrite(kFrame);
    transport.short_write = true;
    EXPECT_THAT(Exchange(), IsErrWith(ErrorKind::kDisconnected, Eq("short K-Line write")));
    EXPECT_EQ(transport.write_calls, 1);
    EXPECT_EQ(transport.read_calls, 0);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST_F(M32rSsmExchangeTest, CancelsBeforeWrite)
{
    cancellation.Cancel();
    EXPECT_THAT(Exchange(), IsErrWith(ErrorKind::kCancelled, Eq("cancelled before write")));
    EXPECT_EQ(transport.write_calls, 0);
    EXPECT_EQ(transport.read_calls, 0);
}

TEST_F(M32rSsmExchangeTest, CancelsAfterWrite)
{
    transport.ExpectWrite(kFrame);
    transport.cancel_on_write = true;
    EXPECT_THAT(Exchange(), IsErrWith(ErrorKind::kCancelled, Eq("cancelled after write")));
    EXPECT_EQ(transport.write_calls, 1);
    EXPECT_EQ(transport.read_calls, 0);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST_F(M32rSsmExchangeTest, CancelsAfterRead)
{
    transport.Exchange(kFrame, bytes::Bytes{0x01});
    transport.cancel_on_read = true;
    EXPECT_THAT(Exchange(), IsErrWith(ErrorKind::kCancelled, Eq("cancelled after read")));
    EXPECT_EQ(transport.write_calls, 1);
    EXPECT_EQ(transport.read_calls, 1);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST_F(M32rSsmExchangeTest, WriteFailureWinsOverCancellation)
{
    transport.ExpectWrite(kFrame);
    transport.cancel_on_write = true;
    transport.write_fault = true;
    EXPECT_THAT(Exchange(), IsErrWith(ErrorKind::kDisconnected, Eq("write fault")));
    EXPECT_EQ(transport.write_calls, 1);
    EXPECT_EQ(transport.read_calls, 0);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST_F(M32rSsmExchangeTest, ReadFailureWinsOverCancellation)
{
    transport.ExpectWrite(kFrame);
    transport.QueueError(ErrorKind::kDisconnected, "read fault");
    transport.cancel_on_read = true;
    EXPECT_THAT(Exchange(), IsErrWith(ErrorKind::kDisconnected, Eq("read fault")));
    EXPECT_EQ(transport.write_calls, 1);
    EXPECT_EQ(transport.read_calls, 1);
    EXPECT_TRUE(transport.ScriptConsumed());
}
} // namespace
