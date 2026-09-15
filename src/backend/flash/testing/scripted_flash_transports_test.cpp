#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"

#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

#include <gtest/gtest.h>

#include <chrono>

namespace fastecu::flash
{
namespace
{

TEST(ScriptedCanFlashTransport, DefaultsClosed)
{
    ScriptedCanFlashTransport transport;

    EXPECT_FALSE(transport.is_open());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(ScriptedCanFlashTransport, ExplicitOpenStateStartsOpenWithoutLifecycleCalls)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};

    EXPECT_TRUE(transport.is_open());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(ScriptedKlineFlashTransport, DefaultsClosed)
{
    ScriptedKlineFlashTransport transport;

    EXPECT_FALSE(transport.isOpen());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(ScriptedKlineFlashTransport, ExplicitOpenStateStartsOpenWithoutLifecycleCalls)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::Open};

    EXPECT_TRUE(transport.isOpen());
    EXPECT_EQ(transport.close_call_count_, 0);
}

TEST(ScriptedCanFlashTransport, ExchangePairsARequestWithItsReply)
{
    ScriptedCanFlashTransport transport;
    const auto section = transport.section("bench connect");
    transport.exchange(bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x50, 0x43});

    FakeCancellationToken cancellation;
    ASSERT_TRUE(transport.write(bytes::Bytes{0x10, 0x43}, cancellation).has_value());
    const auto reply = transport.read(std::chrono::milliseconds{100}, cancellation);

    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(reply->has_value());
    EXPECT_THAT(**reply, test_bytes::BytesEq(bytes::Bytes{0x50, 0x43}));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(ScriptedCanFlashTransport, ADivergentWriteNamesTheStepAndPrintsBothSides)
{
    ScriptedCanFlashTransport transport;
    {
        const auto preamble = transport.section("preliminaries");
        transport.exchange(bytes::Bytes{0x10, 0x5F}, bytes::Bytes{0x50, 0x01});
    }
    const auto section = transport.section("bench connect");
    transport.exchange(bytes::Bytes{0x27, 0x61}, bytes::Bytes{0x67, 0x61});

    FakeCancellationToken cancellation;
    ASSERT_TRUE(transport.write(bytes::Bytes{0x10, 0x5F}, cancellation).has_value());
    const auto status = transport.write(bytes::Bytes{0x27, 0x62}, cancellation);

    ASSERT_FALSE(status.has_value());
    // The index is 1-based over the whole script, so it matches what a reader
    // counts down the file.
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("exchange #2"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("bench connect"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("27 61"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("27 62"));
}

TEST(ScriptedCanFlashTransport, AWriteRunningPastTheScriptSaysSo)
{
    ScriptedCanFlashTransport transport;
    const auto section = transport.section("bench connect");
    transport.exchange(bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x50, 0x43});

    FakeCancellationToken cancellation;
    ASSERT_TRUE(transport.write(bytes::Bytes{0x10, 0x43}, cancellation).has_value());
    const auto status = transport.write(bytes::Bytes{0x37}, cancellation);

    ASSERT_FALSE(status.has_value());
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("past the end of the script"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("37"));
}

TEST(ScriptedCanFlashTransport, StepsOutsideAnySectionStillReportTheirIndex)
{
    ScriptedCanFlashTransport transport;
    transport.exchange(bytes::Bytes{0x37}, bytes::Bytes{0x77});

    FakeCancellationToken cancellation;
    const auto status = transport.write(bytes::Bytes{0x38}, cancellation);

    ASSERT_FALSE(status.has_value());
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("exchange #1"));
}

TEST(ScriptedKlineFlashTransport, ExchangePairsARequestWithItsReply)
{
    ScriptedKlineFlashTransport transport;
    const auto section = transport.section("bench connect");
    transport.exchange(bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x50, 0x43});

    ASSERT_TRUE(transport.write(bytes::Bytes{0x10, 0x43}).has_value());
    FakeCancellationToken cancellation;
    const auto reply = transport.read(std::chrono::milliseconds{100}, cancellation);

    ASSERT_TRUE(reply.has_value());
    ASSERT_TRUE(reply->has_value());
    EXPECT_THAT(**reply, test_bytes::BytesEq(bytes::Bytes{0x50, 0x43}));
    EXPECT_TRUE(transport.scriptConsumed());
}

TEST(ScriptedKlineFlashTransport, ADivergentWriteNamesTheStepAndPrintsBothSides)
{
    ScriptedKlineFlashTransport transport;
    {
        const auto preamble = transport.section("preliminaries");
        transport.exchange(bytes::Bytes{0x10, 0x5F}, bytes::Bytes{0x50, 0x01});
    }
    const auto section = transport.section("bench connect");
    transport.exchange(bytes::Bytes{0x27, 0x61}, bytes::Bytes{0x67, 0x61});

    ASSERT_TRUE(transport.write(bytes::Bytes{0x10, 0x5F}).has_value());
    const auto status = transport.write(bytes::Bytes{0x27, 0x62});

    ASSERT_FALSE(status.has_value());
    // The index is 1-based over the whole script, so it matches what a reader
    // counts down the file.
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("scripted K-Line exchange #2"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("bench connect"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("27 61"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("27 62"));
}

TEST(ScriptedKlineFlashTransport, AWriteRunningPastTheScriptSaysSo)
{
    ScriptedKlineFlashTransport transport;
    const auto section = transport.section("bench connect");
    transport.exchange(bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x50, 0x43});

    ASSERT_TRUE(transport.write(bytes::Bytes{0x10, 0x43}).has_value());
    const auto status = transport.write(bytes::Bytes{0x37});

    ASSERT_FALSE(status.has_value());
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("past the end of the script"));
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("37"));
}

TEST(ScriptedKlineFlashTransport, StepsOutsideAnySectionStillReportTheirIndex)
{
    ScriptedKlineFlashTransport transport;
    transport.exchange(bytes::Bytes{0x37}, bytes::Bytes{0x77});

    const auto status = transport.write(bytes::Bytes{0x38});

    ASSERT_FALSE(status.has_value());
    EXPECT_THAT(status.error().detail, ::testing::HasSubstr("exchange #1"));
}

} // namespace
} // namespace fastecu::flash
