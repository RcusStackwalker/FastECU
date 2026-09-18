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

FlashPlanFields kline_read_fields()
{
    return FlashPlanFields{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromKline,
        .transport = TransportKind::Kline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .transfer_region = MemoryRegion{.start = 0xf000, .length = 0x1000},
        .erase_regions = {},
        .image = std::nullopt,
        .kernel = KernelImage{.id = "k", .load_address = 0xffff2000, .bytes = {0x01}},
        .family_plan =
            DensoSh705xEepromKlinePlan{
                .mode = EepromReadMode::Mode2,
                .security = DensoSecurityVariant::Stock,
                .tester_id = 0xf0,
                .target_id = 0x10,
                .initial_baud = 4800,
                .kernel_baud = 15625,
            },
        .confirmations =
            {
                ConfirmationSpec{.id = ConfirmationSpec::Id::BeginEepromRead},
                ConfirmationSpec{.id = ConfirmationSpec::Id::InspectEepromBytes},
            },
    };
}

FlashPlan built_plan()
{
    auto plan = validate_and_build(kline_read_fields());
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

    Result<MixedCanConfig> transport_setup(const FlashPlan&) const override
    {
        return kMixedSetup;
    }

    Status before_transport_configure(IMixedCanFlashTransport&, IClock&, const ICancellationToken&) const override
    {
        calls_.push_back("before_transport_configure");
        if (!before_configure_ok)
        {
            return fail(ErrorKind::Internal, "before_transport_configure failed");
        }
        return {};
    }

    Result<FlashExecutionResult> execute(const FlashPlan&, IMixedCanFlashTransport&, IClock&, const ICancellationToken&,
                                         IEventSink&) override
    {
        calls_.push_back("execute");
        return FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = bytes::Bytes{0x01}};
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

    Status configure(const MixedCanConfig&) override
    {
        calls_.push_back("configure");
        return {};
    }
    Status reset_connection() override
    {
        calls_.push_back("reset_connection");
        return {};
    }
    Status open() override
    {
        calls_.push_back("open");
        return {};
    }
    Status close() override
    {
        calls_.push_back("close");
        return {};
    }
    Status enter_raw_bootloader_mode() override
    {
        return {};
    }
    Status clear_receive_buffer() override
    {
        return {};
    }
    Status enter_iso15765_kernel_mode() override
    {
        return {};
    }
    Status write_iso15765(bytes::ByteView, const ICancellationToken&) override
    {
        return {};
    }
    Result<std::optional<bytes::Bytes>> read_iso15765(std::chrono::milliseconds, const ICancellationToken&) override
    {
        return std::optional<bytes::Bytes>{};
    }
    Status write_raw(const cdbg::CanFrame&, const ICancellationToken&) override
    {
        return {};
    }
    Result<std::optional<cdbg::CanFrame>> read_raw(std::chrono::milliseconds, const ICancellationToken&) override
    {
        return std::optional<cdbg::CanFrame>{};
    }
    void request_unblock() noexcept override
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
    Result<KlineConfig> transport_setup(const FlashPlan&) const override
    {
        if (!setup_ok)
        {
            return fail(ErrorKind::InvalidConfig, "bad plan");
        }
        return kSetup;
    }

    Result<FlashExecutionResult> execute(const FlashPlan&, IKlineFlashTransport& transport, IClock&,
                                         const ICancellationToken&, IEventSink&) override
    {
        ++execute_calls;
        saw_open_transport = transport.isOpen();
        if (!execute_ok)
        {
            return fail(ErrorKind::BadResponse, "execute failed");
        }
        return FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = bytes::Bytes{0x01}};
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
        attempt = bind_flash_attempt(built_plan(), std::move(owned_executor), std::move(owned_transport));
    }

    Result<FlashExecutionResult> run()
    {
        return attempt->run(clock, cancellation, events);
    }
};

TEST(BoundFlashAttemptTest, ConfiguresAndOpensBeforeExecuteAndClosesOnce)
{
    Harness h;

    ASSERT_THAT(h.run(), fastecu::testing::IsOk());
    ASSERT_TRUE(h.transport->last_config_.has_value());
    EXPECT_EQ(h.transport->last_config_->baud, kSetup.baud);
    EXPECT_EQ(h.transport->last_config_->tester_id, kSetup.tester_id);
    EXPECT_TRUE(h.executor->saw_open_transport);
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, InvalidPlanTouchesNoTransportCall)
{
    Harness h;
    h.executor->setup_ok = false;

    ASSERT_THAT(h.run(), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
    EXPECT_FALSE(h.transport->last_config_.has_value());
    EXPECT_EQ(h.transport->close_call_count_, 0);
    EXPECT_EQ(h.executor->execute_calls, 0);
}

TEST(BoundFlashAttemptTest, ConfigureFailureSkipsOpenAndExecuteAndClose)
{
    Harness h;
    h.transport->configure_result_ = fail(ErrorKind::Disconnected, "no port");

    ASSERT_THAT(h.run(), fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count_, 0);
}

TEST(BoundFlashAttemptTest, OpenFailureSkipsExecuteAndClose)
{
    Harness h;
    h.transport->open_result_ = fail(ErrorKind::Disconnected, "open failed");

    ASSERT_THAT(h.run(), fastecu::testing::IsErr(ErrorKind::Disconnected));
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count_, 0);
}

TEST(BoundFlashAttemptTest, CancelledBeforeConfigureDoesNotConfigure)
{
    Harness h;
    h.cancellation.set_cancelled(true);

    ASSERT_THAT(h.run(), fastecu::testing::IsErr(ErrorKind::Cancelled));
    EXPECT_FALSE(h.transport->last_config_.has_value());
}

TEST(BoundFlashAttemptTest, CancellationIsNotUniversallyPolledBetweenConfigureAndOpen)
{
    Harness h;
    h.cancellation.cancel_on_check(2);

    ASSERT_THAT(h.run(), fastecu::testing::IsOk());
    EXPECT_TRUE(h.transport->last_config_.has_value());
    EXPECT_EQ(h.executor->execute_calls, 1);
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorIsReturnedAndTransportStillClosesOnce)
{
    Harness h;
    h.executor->execute_ok = false;

    ASSERT_THAT(h.run(), fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, CloseOnlyErrorIsReturned)
{
    Harness h;
    h.transport->close_result_ = fail(ErrorKind::Internal, "close failed");

    ASSERT_THAT(h.run(), fastecu::testing::IsErr(ErrorKind::Internal));
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorWinsOverCloseErrorAndCloseIsLogged)
{
    Harness h;
    h.executor->execute_ok = false;
    h.transport->close_result_ = fail(ErrorKind::Internal, "close failed");

    ASSERT_THAT(h.run(), fastecu::testing::IsErr(ErrorKind::BadResponse));
    EXPECT_EQ(h.transport->close_call_count_, 1);
    const bool warned =
        std::ranges::any_of(h.events.logs, [](const auto& entry) { return entry.first == LogLevel::Warning; });
    EXPECT_TRUE(warned);
}

TEST(BoundFlashAttemptTest, RequestUnblockReachesTheTransport)
{
    using namespace std::chrono_literals;

    Harness h;
    h.transport->queueBlockingRead();

    h.attempt->request_unblock();

    auto read = h.transport->read(10ms, h.cancellation);
    ASSERT_THAT(read, fastecu::testing::IsErr(ErrorKind::Cancelled));
}

TEST(BoundFlashAttemptTest, MixedTransportUsesOuterLifecycleExactlyOnce)
{
    std::vector<std::string> calls;
    auto attempt = bind_flash_attempt(built_plan(), std::make_unique<TestMixedExecutor>(calls),
                                      std::make_unique<RecordingMixedTransport>(calls));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_TRUE(attempt->run(clock, cancellation, events).has_value());
    EXPECT_THAT(calls, ::testing::ElementsAre("before_transport_configure", "configure", "open", "execute", "close"));
}

TEST(BoundFlashAttemptTest, MixedTransportBeforeConfigureErrorSkipsConfigureOpenExecuteClose)
{
    std::vector<std::string> calls;
    auto executor = std::make_unique<TestMixedExecutor>(calls);
    executor->before_configure_ok = false;
    auto attempt =
        bind_flash_attempt(built_plan(), std::move(executor), std::make_unique<RecordingMixedTransport>(calls));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(attempt->run(clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::Internal));
    EXPECT_THAT(calls, ::testing::ElementsAre("before_transport_configure"));
    EXPECT_TRUE(std::ranges::none_of(calls, [](const std::string& call) { return call == "configure"; }));
}

} // namespace
} // namespace fastecu::flash
