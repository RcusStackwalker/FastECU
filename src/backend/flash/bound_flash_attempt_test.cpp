#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/flash_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <concepts>
#include <string>
#include <vector>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash
{
namespace
{

FlashPlanFields KlineReadFields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
        .family = FlashFamily::kDensoSh705xEepromKline,
        .transport = TransportKind::kKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0xf000, .length = 0x1000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff2000, .bytes = {0x01}},
        .family_plan =
            DensoSh705xEepromKlinePlan{
                .mode = EepromReadMode::kMode2,
                .security = DensoSecurityVariant::kStock,
                .tester_id = 0xf0,
                .target_id = 0x10,
                .initial_baud = 4800,
                .kernel_baud = 15625,
            },
        .confirmations =
            {
                ConfirmationSpec{.id = ConfirmationSpec::Id::kBeginEepromRead},
                ConfirmationSpec{.id = ConfirmationSpec::Id::kInspectEepromBytes},
            },
    };
}

FlashPlan BuiltPlan()
{
    auto plan = ValidateAndBuild(KlineReadFields());
    EXPECT_THAT(plan, fastecu::testing::IsOk());
    return std::move(*plan);
}

constexpr KlineConfig kSetup{.baud = 4800, .iso14230 = false, .tester_id = 0xf0, .target_id = 0x10};

constexpr MixedCanConfig kMixedSetup{
    .kernel = Iso15765Config{.bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false},
    .bootloader =
        RawCanConfig{.bitrate = 500000, .transmit_id = 0x000ffffe, .receive_id = 0x000fffff, .extended_id = true},
};

class TestMixedExecutor final : public IMixedCanFlashExecutor
{
  public:
    explicit TestMixedExecutor(std::vector<std::string>& calls) : calls_(calls)
    {
    }

    Result<MixedCanConfig> TransportSetup(const FlashPlan&) const override
    {
        return kMixedSetup;
    }

    Status BeforeTransportConfigure(IMixedCanFlashTransport&, IClock&, const ICancellationToken&) const override
    {
        calls_.push_back("before_transport_configure");
        if (!before_configure_ok)
        {
            return Fail(ErrorKind::kInternal, "before_transport_configure failed");
        }
        return {};
    }

    Result<FlashExecutionResult> Execute(const FlashPlan&, IMixedCanFlashTransport&, IClock&, const ICancellationToken&,
                                         IEventSink&) override
    {
        calls_.push_back("execute");
        return FlashExecutionResult{.operation = FlashOperation::kRead, .read_bytes = bytes::Bytes{0x01}};
    }

    bool before_configure_ok = true;

  private:
    std::vector<std::string>& calls_;
};

class RecordingMixedTransport final : public IMixedCanFlashTransport
{
  public:
    explicit RecordingMixedTransport(std::vector<std::string>& calls) : calls_(calls)
    {
    }

    Status Configure(const MixedCanConfig&) override
    {
        calls_.push_back("configure");
        return {};
    }
    Status ResetConnection() override
    {
        calls_.push_back("reset_connection");
        return {};
    }
    Status Open() override
    {
        calls_.push_back("open");
        return {};
    }
    Status Close() override
    {
        calls_.push_back("close");
        return {};
    }
    Status EnterRawBootloaderMode() override
    {
        return {};
    }
    Status ClearReceiveBuffer() override
    {
        return {};
    }
    Status EnterIso15765KernelMode() override
    {
        return {};
    }
    Status WriteIso15765(bytes::ByteView, const ICancellationToken&) override
    {
        return {};
    }
    Result<std::optional<bytes::Bytes>> ReadIso15765(std::chrono::milliseconds, const ICancellationToken&) override
    {
        return std::optional<bytes::Bytes>{};
    }
    Status WriteRaw(const cdbg::CanFrame&, const ICancellationToken&) override
    {
        return {};
    }
    Result<std::optional<cdbg::CanFrame>> ReadRaw(std::chrono::milliseconds, const ICancellationToken&) override
    {
        return std::optional<cdbg::CanFrame>{};
    }
    void RequestUnblock() noexcept override
    {
    }

  private:
    std::vector<std::string>& calls_;
};

static_assert(std::derived_from<TestMixedExecutor::TransportType, IMixedCanFlashTransport>);
static_assert(!std::derived_from<ICanFlashTransport, IMixedCanFlashTransport>);

class FakeKlineExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> TransportSetup(const FlashPlan&) const override
    {
        if (!setup_ok)
        {
            return Fail(ErrorKind::kInvalidConfig, "bad plan");
        }
        return kSetup;
    }

    Result<FlashExecutionResult> Execute(const FlashPlan&, IKlineFlashTransport& transport, IClock&,
                                         const ICancellationToken&, IEventSink&) override
    {
        ++execute_calls;
        saw_open_transport = transport.IsOpen();
        if (!execute_ok)
        {
            return Fail(ErrorKind::kBadResponse, "execute failed");
        }
        return FlashExecutionResult{.operation = FlashOperation::kRead, .read_bytes = bytes::Bytes{0x01}};
    }

    bool setup_ok = true;
    bool execute_ok = true;
    int execute_calls = 0;
    bool saw_open_transport = false;
};

struct Harness
{
    ScriptedKlineFlashTransport *transport = nullptr;
    FakeKlineExecutor *executor = nullptr;
    std::unique_ptr<BoundFlashAttempt> attempt;
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    Harness()
    {
        auto owned_transport = std::make_unique<ScriptedKlineFlashTransport>();
        auto owned_executor = std::make_unique<FakeKlineExecutor>();
        transport = owned_transport.get();
        executor = owned_executor.get();
        attempt = BindFlashAttempt(BuiltPlan(), std::move(owned_executor), std::move(owned_transport));
    }

    Result<FlashExecutionResult> Run()
    {
        return attempt->Run(clock, cancellation, events);
    }
};

TEST(BoundFlashAttemptTest, ConfiguresAndOpensBeforeExecuteAndClosesOnce)
{
    Harness h;

    ASSERT_THAT(h.Run(), fastecu::testing::IsOk());
    ASSERT_TRUE(h.transport->last_config.has_value());
    EXPECT_EQ(h.transport->last_config->baud, kSetup.baud);
    EXPECT_EQ(h.transport->last_config->tester_id, kSetup.tester_id);
    EXPECT_TRUE(h.executor->saw_open_transport);
    EXPECT_EQ(h.transport->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, InvalidPlanTouchesNoTransportCall)
{
    Harness h;
    h.executor->setup_ok = false;

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_FALSE(h.transport->last_config.has_value());
    EXPECT_EQ(h.transport->close_call_count, 0);
    EXPECT_EQ(h.executor->execute_calls, 0);
}

TEST(BoundFlashAttemptTest, ConfigureFailureSkipsOpenAndExecuteAndClose)
{
    Harness h;
    h.transport->configure_result = Fail(ErrorKind::kDisconnected, "no port");

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count, 0);
}

TEST(BoundFlashAttemptTest, OpenFailureSkipsExecuteAndClose)
{
    Harness h;
    h.transport->open_result = Fail(ErrorKind::kDisconnected, "open failed");

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count, 0);
}

TEST(BoundFlashAttemptTest, CancelledBeforeConfigureDoesNotConfigure)
{
    Harness h;
    h.cancellation.SetCancelled(true);

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_FALSE(h.transport->last_config.has_value());
}

TEST(BoundFlashAttemptTest, CancellationIsNotUniversallyPolledBetweenConfigureAndOpen)
{
    Harness h;
    h.cancellation.CancelOnCheck(2);

    ASSERT_THAT(h.Run(), fastecu::testing::IsOk());
    EXPECT_TRUE(h.transport->last_config.has_value());
    EXPECT_EQ(h.executor->execute_calls, 1);
    EXPECT_EQ(h.transport->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorIsReturnedAndTransportStillClosesOnce)
{
    Harness h;
    h.executor->execute_ok = false;

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(h.transport->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, CloseOnlyErrorIsReturned)
{
    Harness h;
    h.transport->close_result = Fail(ErrorKind::kInternal, "close failed");

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_EQ(h.transport->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorWinsOverCloseErrorAndCloseIsLogged)
{
    Harness h;
    h.executor->execute_ok = false;
    h.transport->close_result = Fail(ErrorKind::kInternal, "close failed");

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(h.transport->close_call_count, 1);
    const bool warned =
        std::ranges::any_of(h.events.logs, [](const auto& entry) { return entry.first == LogLevel::kWarning; });
    EXPECT_TRUE(warned);
}

TEST(BoundFlashAttemptTest, RequestUnblockReachesTheTransport)
{
    using namespace std::chrono_literals;

    Harness h;
    h.transport->QueueBlockingRead();

    h.attempt->RequestUnblock();

    auto read = h.transport->Read(10ms, h.cancellation);
    ASSERT_THAT(read, fastecu::testing::IsErr(ErrorKind::kCancelled));
}

TEST(BoundFlashAttemptTest, MixedTransportUsesOuterLifecycleExactlyOnce)
{
    std::vector<std::string> calls;
    auto attempt = BindFlashAttempt(BuiltPlan(), std::make_unique<TestMixedExecutor>(calls),
                                    std::make_unique<RecordingMixedTransport>(calls));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_TRUE(attempt->Run(clock, cancellation, events).has_value());
    EXPECT_THAT(calls, ::testing::ElementsAre("before_transport_configure", "configure", "open", "execute", "close"));
}

TEST(BoundFlashAttemptTest, MixedTransportBeforeConfigureErrorSkipsConfigureOpenExecuteClose)
{
    std::vector<std::string> calls;
    auto executor = std::make_unique<TestMixedExecutor>(calls);
    executor->before_configure_ok = false;
    auto attempt = BindFlashAttempt(BuiltPlan(), std::move(executor), std::make_unique<RecordingMixedTransport>(calls));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(attempt->Run(clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_THAT(calls, ::testing::ElementsAre("before_transport_configure"));
    EXPECT_TRUE(std::ranges::none_of(calls, [](const std::string& call) { return call == "configure"; }));
}

} // namespace
} // namespace fastecu::flash
