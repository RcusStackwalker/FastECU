#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <string_view>
#include <utility>
#include <vector>

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

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

TEST(ScriptedCanFlashTransportTest, RestartUsesMandatoryResetThenExactConfigurationThenOpen)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
    FakeCancellationToken cancellation;
    const Iso15765Config restart_config{
        .bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false};

    const auto result = transport.restart_iso15765(restart_config, cancellation);

    ASSERT_TRUE(result.has_value());
    EXPECT_THAT(transport.lifecycle_calls_, testing::ElementsAre("reset_connection", "configure", "open"));
    ASSERT_TRUE(transport.last_config_.has_value());
    EXPECT_EQ(transport.last_config_->bitrate, 500000);
    EXPECT_EQ(transport.last_config_->request_id, 0x7e1U);
    EXPECT_EQ(transport.last_config_->response_id, 0x7e9U);
    EXPECT_FALSE(transport.last_config_->extended_id);
}

TEST(ScriptedCanFlashTransportTest, RestartStopsAtEachFailureAndPreservesTheExactError)
{
    const std::array failure_cases{
        std::pair{"reset", ErrorKind::Internal},
        std::pair{"configure", ErrorKind::InvalidConfig},
        std::pair{"open", ErrorKind::Disconnected},
    };
    for (const auto& [stage, expected_kind] : failure_cases)
    {
        SCOPED_TRACE(stage);
        ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
        FakeCancellationToken cancellation;
        if (stage == std::string_view{"reset"})
        {
            transport.reset_result_ = fail(expected_kind, "reset marker");
        }
        else if (stage == std::string_view{"configure"})
        {
            transport.configure_result_ = fail(expected_kind, "configure marker");
        }
        else
        {
            transport.open_result_ = fail(expected_kind, "open marker");
        }

        const auto result = transport.restart_iso15765(
            {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, expected_kind);
        if (stage == std::string_view{"reset"})
        {
            EXPECT_THAT(transport.lifecycle_calls_, testing::ElementsAre("reset_connection"));
        }
        else if (stage == std::string_view{"configure"})
        {
            EXPECT_THAT(transport.lifecycle_calls_, testing::ElementsAre("reset_connection", "configure"));
        }
        else
        {
            EXPECT_THAT(transport.lifecycle_calls_, testing::ElementsAre("reset_connection", "configure", "open"));
        }
    }
}

TEST(ScriptedCanFlashTransportTest, RestartChecksCancellationAtEveryLogicalBoundary)
{
    class BoundaryCancellation final : public ICancellationToken
    {
      public:
        explicit BoundaryCancellation(int cancel_on_check) : cancel_on_check_(cancel_on_check)
        {
        }
        bool cancelled() const override
        {
            return ++checks_ >= cancel_on_check_;
        }

      private:
        int cancel_on_check_;
        mutable int checks_{};
    };

    const std::array expected_calls{
        std::vector<std::string>{},
        std::vector<std::string>{"reset_connection"},
        std::vector<std::string>{"reset_connection", "configure"},
        std::vector<std::string>{"reset_connection", "configure", "open"},
    };
    for (std::size_t boundary = 0; boundary < expected_calls.size(); ++boundary)
    {
        SCOPED_TRACE(boundary);
        ScriptedCanFlashTransport transport{ScriptedTransportInitialState::Open};
        BoundaryCancellation cancellation(static_cast<int>(boundary + 1));

        const auto result = transport.restart_iso15765(
            {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
        EXPECT_EQ(transport.lifecycle_calls_, expected_calls[boundary]);
    }
}

TEST(ScriptedKlineFlashTransportTest, DefaultsClosed)
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
