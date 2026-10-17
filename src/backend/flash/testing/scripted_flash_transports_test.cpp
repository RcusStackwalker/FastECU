#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/flash/testing/scripted_mixed_can_flash_transport.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/result_matchers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <string_view>
#include <utility>
#include <vector>

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;
using fastecu::testing::IsErr;
using fastecu::testing::IsErrWith;
using fastecu::testing::IsOk;

TEST(ScriptedKlineFlashTransport, RawExpectationsRejectFramedCalls)
{
    ScriptedKlineFlashTransport transport;
    FakeCancellationToken cancellation;
    const bytes::Bytes request{0xaa};
    const bytes::Bytes reply{0x00, 0xff};
    transport.ExpectRawWrite(request);
    EXPECT_THAT(transport.Write(request), IsErr(ErrorKind::kInternal));
    EXPECT_THAT(transport.WriteRaw(request), IsOk());
    transport.QueueRawRead(reply);
    EXPECT_THAT(transport.Read(10ms, cancellation), IsErr(ErrorKind::kInternal));
    const auto result = transport.ReadRaw(10ms, cancellation);
    ASSERT_THAT(result, IsOk());
    ASSERT_TRUE(result->has_value());
    EXPECT_THAT(result->value(), ::testing::ElementsAre(0x00, 0xff));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(ScriptedCanFlashTransport, DefaultsClosed)
{
    ScriptedCanFlashTransport transport;

    EXPECT_FALSE(transport.IsOpen());
    EXPECT_EQ(transport.close_call_count, 0);
}

TEST(ScriptedCanFlashTransport, ExplicitOpenStateStartsOpenWithoutLifecycleCalls)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};

    EXPECT_TRUE(transport.IsOpen());
    EXPECT_EQ(transport.close_call_count, 0);
}

TEST(ScriptedCanFlashTransport, RestartUsesMandatoryResetThenExactConfigurationThenOpen)
{
    ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
    FakeCancellationToken cancellation;
    const Iso15765Config restart_config{
        .bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false};

    const auto result = transport.RestartIso15765(restart_config, cancellation);

    EXPECT_THAT(result, IsOk());
    EXPECT_THAT(transport.lifecycle_calls, ::testing::ElementsAre("reset_connection", "configure", "open"));
    ASSERT_TRUE(transport.last_config.has_value());
    EXPECT_EQ(transport.last_config->bitrate, 500000);
    EXPECT_EQ(transport.last_config->request_id, 0x7e1U);
    EXPECT_EQ(transport.last_config->response_id, 0x7e9U);
    EXPECT_FALSE(transport.last_config->extended_id);
}

TEST(ScriptedCanFlashTransport, RestartStopsAtEachFailureAndPreservesTheExactError)
{
    const std::array failure_cases{
        std::pair{"reset", ErrorKind::kInternal},
        std::pair{"configure", ErrorKind::kInvalidConfig},
        std::pair{"open", ErrorKind::kDisconnected},
    };
    for (const auto& [stage, expected_kind] : failure_cases)
    {
        SCOPED_TRACE(stage);
        ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
        FakeCancellationToken cancellation;
        if (stage == std::string_view{"reset"})
        {
            transport.reset_result = Fail(expected_kind, "reset marker");
        }
        else if (stage == std::string_view{"configure"})
        {
            transport.configure_result = Fail(expected_kind, "configure marker");
        }
        else
        {
            transport.open_result = Fail(expected_kind, "open marker");
        }

        const auto result = transport.RestartIso15765(
            {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

        EXPECT_THAT(result, IsErr(expected_kind));
        if (stage == std::string_view{"reset"})
        {
            EXPECT_THAT(transport.lifecycle_calls, ::testing::ElementsAre("reset_connection"));
        }
        else if (stage == std::string_view{"configure"})
        {
            EXPECT_THAT(transport.lifecycle_calls, ::testing::ElementsAre("reset_connection", "configure"));
        }
        else
        {
            EXPECT_THAT(transport.lifecycle_calls, ::testing::ElementsAre("reset_connection", "configure", "open"));
        }
    }
}

TEST(ScriptedCanFlashTransport, RestartChecksCancellationAtEveryLogicalBoundary)
{
    class BoundaryCancellation final : public ICancellationToken
    {
      public:
        explicit BoundaryCancellation(int cancel_on_check) : cancel_on_check_(cancel_on_check)
        {
        }
        bool Cancelled() const override
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
        ScriptedCanFlashTransport transport{ScriptedTransportInitialState::kOpen};
        BoundaryCancellation cancellation(static_cast<int>(boundary + 1));

        const auto result = transport.RestartIso15765(
            {.bitrate = 500000, .request_id = 0x7e1, .response_id = 0x7e9, .extended_id = false}, cancellation);

        EXPECT_THAT(result, IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(transport.lifecycle_calls, expected_calls[boundary]);
    }
}

TEST(ScriptedKlineFlashTransport, DefaultsClosed)
{
    ScriptedKlineFlashTransport transport;

    EXPECT_FALSE(transport.IsOpen());
    EXPECT_EQ(transport.close_call_count, 0);
}

TEST(ScriptedKlineFlashTransport, ExplicitOpenStateStartsOpenWithoutLifecycleCalls)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};

    EXPECT_TRUE(transport.IsOpen());
    EXPECT_EQ(transport.close_call_count, 0);
}

TEST(ScriptedKlineFlashTransport, RecordsResetInLifecycleOrderAndClosesThePort)
{
    ScriptedKlineFlashTransport transport{ScriptedTransportInitialState::kOpen};

    ASSERT_THAT(transport.ResetConnection(), IsOk());
    EXPECT_FALSE(transport.IsOpen());
    ASSERT_THAT(transport.Configure(KlineConfig{.baud = 4800, .iso14230 = false, .tester_id = 0xF0, .target_id = 0x10}),
                IsOk());
    ASSERT_THAT(transport.Open(), IsOk());
    ASSERT_THAT(transport.Close(), IsOk());

    EXPECT_EQ(transport.reset_call_count, 1);
    EXPECT_THAT(transport.lifecycle_calls, ::testing::ElementsAre("reset_connection", "configure", "open", "close"));
}

TEST(ScriptedKlineFlashTransport, ResetReturnsItsScriptedFailure)
{
    ScriptedKlineFlashTransport transport;
    transport.reset_result = Fail(ErrorKind::kDisconnected, "scripted reset failure");

    const Status reset = transport.ResetConnection();

    EXPECT_THAT(reset, IsErr(ErrorKind::kDisconnected));
}

constexpr MixedCanConfig kMixedCanConfig{
    .kernel = Iso15765Config{.bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false},
    .bootloader =
        RawCanConfig{.bitrate = 500000, .transmit_id = 0x000ffffe, .receive_id = 0x000fffff, .extended_id = true},
};

void ConfigureAndOpen(ScriptedMixedCanFlashTransport& transport)
{
    ASSERT_THAT(transport.Configure(kMixedCanConfig), IsOk());
    ASSERT_THAT(transport.Open(), IsOk());
}

TEST(ScriptedMixedCanFlashTransport, ScriptsIsoAndRawFramesInTheirRespectiveModes)
{
    ScriptedMixedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    const bytes::Bytes iso_request{0x00, 0x00, 0x07, 0xe0, 0xbe, 0xef};
    transport.ExpectIsoWrite(iso_request);
    transport.QueueIsoRead({0x00, 0x00, 0x07, 0xe8, 0xbe, 0xef, 0x00, 0x00, 0x41});
    transport.ExpectRawWrite(cdbg::CanFrame{0x000ffffe, {0x7a, 0x90, 0, 0, 0, 0, 0, 0}});
    transport.QueueRawRead(cdbg::CanFrame{0x000fffff, {0x7a, 0x91}});

    ConfigureAndOpen(transport);
    ASSERT_THAT(transport.WriteIso15765(iso_request, cancellation), IsOk());
    const auto iso_reply = transport.ReadIso15765(10ms, cancellation);
    ASSERT_THAT(iso_reply, IsOk());
    ASSERT_TRUE(iso_reply->has_value());
    EXPECT_THAT(**iso_reply, ::testing::ElementsAre(0x00, 0x00, 0x07, 0xe8, 0xbe, 0xef, 0x00, 0x00, 0x41));

    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
    ASSERT_THAT(transport.ClearReceiveBuffer(), IsOk());
    ASSERT_THAT(transport.WriteRaw(cdbg::CanFrame{0x000ffffe, {0x7a, 0x90, 0, 0, 0, 0, 0, 0}}, cancellation), IsOk());
    const auto raw_reply = transport.ReadRaw(10ms, cancellation);
    ASSERT_THAT(raw_reply, IsOk());
    ASSERT_TRUE(raw_reply->has_value());
    EXPECT_EQ((*raw_reply)->id, 0x000fffffU);
    EXPECT_THAT((*raw_reply)->payload, ::testing::ElementsAre(0x7a, 0x91));
    EXPECT_TRUE(transport.ScriptConsumed());
    EXPECT_THAT(transport.ModeChanges(),
                ::testing::ElementsAre(ScriptedMixedCanMode::kIso15765Kernel, ScriptedMixedCanMode::kRawBootloader));
    EXPECT_EQ(transport.ConfigureCallCount(), 1);
    EXPECT_EQ(transport.OpenCallCount(), 1);
    EXPECT_EQ(transport.CloseCallCount(), 0);
}

TEST(ScriptedMixedCanFlashTransport, RejectsWrongModeAndOversizeRawPayload)
{
    ScriptedMixedCanFlashTransport transport;
    FakeCancellationToken cancellation;

    ConfigureAndOpen(transport);
    const auto raw_in_iso_mode = transport.WriteRaw(cdbg::CanFrame{0x000ffffe, {0x7a}}, cancellation);
    EXPECT_THAT(raw_in_iso_mode, IsErr(ErrorKind::kInvalidConfig));

    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());
    const bytes::Bytes iso_request{0x01};
    const auto iso_in_raw_mode = transport.WriteIso15765(iso_request, cancellation);
    EXPECT_THAT(iso_in_raw_mode, IsErr(ErrorKind::kInvalidConfig));

    const auto oversized_raw = transport.WriteRaw(cdbg::CanFrame{0x000ffffe, bytes::Bytes(9, 0x00)}, cancellation);
    EXPECT_THAT(oversized_raw, IsErr(ErrorKind::kInvalidConfig));
}

TEST(ScriptedMixedCanFlashTransport, CancelledReadLeavesItsIsoScriptItemQueued)
{
    ScriptedMixedCanFlashTransport transport;
    FakeCancellationToken cancellation{true};
    transport.QueueIsoRead({0x7f});

    ConfigureAndOpen(transport);
    const auto reply = transport.ReadIso15765(10ms, cancellation);

    EXPECT_THAT(reply, IsErr(ErrorKind::kCancelled));
    EXPECT_FALSE(transport.ScriptConsumed());
}

TEST(ScriptedMixedCanFlashTransport, TransitionFailuresAreOneShot)
{
    ScriptedMixedCanFlashTransport transport;

    ConfigureAndOpen(transport);
    transport.FailNextRawTransition();
    const auto raw_transition = transport.EnterRawBootloaderMode();
    EXPECT_THAT(raw_transition, IsErr(ErrorKind::kInternal));
    ASSERT_THAT(transport.EnterRawBootloaderMode(), IsOk());

    transport.FailNextIsoTransition();
    const auto iso_transition = transport.EnterIso15765KernelMode();
    EXPECT_THAT(iso_transition, IsErr(ErrorKind::kInternal));
    ASSERT_THAT(transport.EnterIso15765KernelMode(), IsOk());
}

TEST(ScriptedMixedCanFlashTransport, RequestUnblockReleasesBlockingIsoReadAsCancelled)
{
    ScriptedMixedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    ConfigureAndOpen(transport);
    transport.QueueBlockingIsoRead();

    auto pending_read = std::async(std::launch::async, [&] { return transport.ReadIso15765(10ms, cancellation); });
    ASSERT_TRUE(transport.WaitUntilBlockingIsoReadEntered(std::chrono::seconds(1)));
    transport.RequestUnblock();
    const auto reply = pending_read.get();

    EXPECT_THAT(reply, IsErr(ErrorKind::kCancelled));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(ScriptedCanFlashTransport, ExchangePairsARequestWithItsReply)
{
    ScriptedCanFlashTransport transport;
    const auto section = transport.Section("bench connect");
    transport.Exchange(bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x50, 0x43});

    FakeCancellationToken cancellation;
    ASSERT_THAT(transport.Write(bytes::Bytes{0x10, 0x43}, cancellation), IsOk());
    const auto reply = transport.Read(std::chrono::milliseconds{100}, cancellation);

    ASSERT_THAT(reply, IsOk());
    ASSERT_TRUE(reply->has_value());
    EXPECT_THAT(**reply, test_bytes::BytesEq(bytes::Bytes{0x50, 0x43}));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(ScriptedCanFlashTransport, DifferentLengthWritesDoNotConsumeTheExpectation)
{
    ScriptedCanFlashTransport transport;
    FakeCancellationToken cancellation;
    transport.ExpectWrite(bytes::Bytes{0x27, 0x62});
    for (const auto& wrong : {bytes::Bytes{}, bytes::Bytes{0x27}, bytes::Bytes{0x27, 0x62, 0x00}})
    {
        SCOPED_TRACE(::testing::PrintToString(wrong));
        EXPECT_THAT(transport.Write(wrong, cancellation), IsErr(ErrorKind::kInternal));
        EXPECT_EQ(transport.WritesConsumed(), 0U);
    }
    EXPECT_THAT(transport.Write(bytes::Bytes{0x27, 0x62}, cancellation), IsOk());
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(ScriptedCanFlashTransport, ADivergentWriteNamesTheStepAndPrintsBothSides)
{
    ScriptedCanFlashTransport transport;
    {
        const auto preamble = transport.Section("preliminaries");
        transport.Exchange(bytes::Bytes{0x10, 0x5F}, bytes::Bytes{0x50, 0x01});
    }
    const auto section = transport.Section("bench connect");
    transport.Exchange(bytes::Bytes{0x27, 0x61}, bytes::Bytes{0x67, 0x61});

    FakeCancellationToken cancellation;
    ASSERT_THAT(transport.Write(bytes::Bytes{0x10, 0x5F}, cancellation), IsOk());
    const auto status = transport.Write(bytes::Bytes{0x27, 0x62}, cancellation);

    ASSERT_THAT(status, IsErr(ErrorKind::kInternal));
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
    const auto section = transport.Section("bench connect");
    transport.Exchange(bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x50, 0x43});

    FakeCancellationToken cancellation;
    ASSERT_THAT(transport.Write(bytes::Bytes{0x10, 0x43}, cancellation), IsOk());
    const auto status = transport.Write(bytes::Bytes{0x37}, cancellation);

    EXPECT_THAT(status,
                IsErrWith(ErrorKind::kInternal, ::testing::AllOf(::testing::HasSubstr("past the end of the script"),
                                                                 ::testing::HasSubstr("37"))));
}

TEST(ScriptedCanFlashTransport, StepsOutsideAnySectionStillReportTheirIndex)
{
    ScriptedCanFlashTransport transport;
    transport.Exchange(bytes::Bytes{0x37}, bytes::Bytes{0x77});

    FakeCancellationToken cancellation;
    const auto status = transport.Write(bytes::Bytes{0x38}, cancellation);

    EXPECT_THAT(status, IsErrWith(ErrorKind::kInternal, ::testing::HasSubstr("exchange #1")));
}

TEST(ScriptedKlineFlashTransport, ExchangePairsARequestWithItsReply)
{
    ScriptedKlineFlashTransport transport;
    const auto section = transport.Section("bench connect");
    transport.Exchange(bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x50, 0x43});

    ASSERT_THAT(transport.Write(bytes::Bytes{0x10, 0x43}), IsOk());
    FakeCancellationToken cancellation;
    const auto reply = transport.Read(std::chrono::milliseconds{100}, cancellation);

    ASSERT_THAT(reply, IsOk());
    ASSERT_TRUE(reply->has_value());
    EXPECT_THAT(**reply, test_bytes::BytesEq(bytes::Bytes{0x50, 0x43}));
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(ScriptedKlineFlashTransport, ADivergentWriteNamesTheStepAndPrintsBothSides)
{
    ScriptedKlineFlashTransport transport;
    {
        const auto preamble = transport.Section("preliminaries");
        transport.Exchange(bytes::Bytes{0x10, 0x5F}, bytes::Bytes{0x50, 0x01});
    }
    const auto section = transport.Section("bench connect");
    transport.Exchange(bytes::Bytes{0x27, 0x61}, bytes::Bytes{0x67, 0x61});

    ASSERT_THAT(transport.Write(bytes::Bytes{0x10, 0x5F}), IsOk());
    const auto status = transport.Write(bytes::Bytes{0x27, 0x62});

    ASSERT_THAT(status, IsErr(ErrorKind::kInternal));
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
    const auto section = transport.Section("bench connect");
    transport.Exchange(bytes::Bytes{0x10, 0x43}, bytes::Bytes{0x50, 0x43});

    ASSERT_THAT(transport.Write(bytes::Bytes{0x10, 0x43}), IsOk());
    const auto status = transport.Write(bytes::Bytes{0x37});

    EXPECT_THAT(status,
                IsErrWith(ErrorKind::kInternal, ::testing::AllOf(::testing::HasSubstr("past the end of the script"),
                                                                 ::testing::HasSubstr("37"))));
}

TEST(ScriptedKlineFlashTransport, StepsOutsideAnySectionStillReportTheirIndex)
{
    ScriptedKlineFlashTransport transport;
    transport.Exchange(bytes::Bytes{0x37}, bytes::Bytes{0x77});

    const auto status = transport.Write(bytes::Bytes{0x38});

    EXPECT_THAT(status, IsErrWith(ErrorKind::kInternal, ::testing::HasSubstr("exchange #1")));
}
} // namespace
} // namespace fastecu::flash
