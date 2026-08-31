#include "src/backend/flash/flash_executor.h"

#include <gtest/gtest.h>

#include <algorithm>

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
    EXPECT_TRUE(plan.has_value());
    return std::move(*plan);
}

constexpr KlineConfig kSetup{.baud = 4800, .iso14230 = false, .tester_id = 0xf0, .target_id = 0x10};

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

    auto result = h.run();

    ASSERT_TRUE(result.has_value());
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

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
    EXPECT_FALSE(h.transport->last_config_.has_value());
    EXPECT_EQ(h.transport->close_call_count_, 0);
    EXPECT_EQ(h.executor->execute_calls, 0);
}

TEST(BoundFlashAttemptTest, ConfigureFailureSkipsOpenAndExecuteAndClose)
{
    Harness h;
    h.transport->configure_result_ = fail(ErrorKind::Disconnected, "no port");

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count_, 0);
}

TEST(BoundFlashAttemptTest, OpenFailureSkipsExecuteAndClose)
{
    Harness h;
    h.transport->open_result_ = fail(ErrorKind::Disconnected, "open failed");

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Disconnected);
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count_, 0);
}

TEST(BoundFlashAttemptTest, CancelledBeforeConfigureDoesNotConfigure)
{
    Harness h;
    h.cancellation.set_cancelled(true);

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Cancelled);
    EXPECT_FALSE(h.transport->last_config_.has_value());
}

TEST(BoundFlashAttemptTest, CancellationIsNotUniversallyPolledBetweenConfigureAndOpen)
{
    Harness h;
    h.cancellation.cancel_on_check(2);

    auto result = h.run();

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(h.transport->last_config_.has_value());
    EXPECT_EQ(h.executor->execute_calls, 1);
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorIsReturnedAndTransportStillClosesOnce)
{
    Harness h;
    h.executor->execute_ok = false;

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, CloseOnlyErrorIsReturned)
{
    Harness h;
    h.transport->close_result_ = fail(ErrorKind::Internal, "close failed");

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::Internal);
    EXPECT_EQ(h.transport->close_call_count_, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorWinsOverCloseErrorAndCloseIsLogged)
{
    Harness h;
    h.executor->execute_ok = false;
    h.transport->close_result_ = fail(ErrorKind::Internal, "close failed");

    auto result = h.run();

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::BadResponse);
    EXPECT_EQ(h.transport->close_call_count_, 1);
    const bool warned =
        std::ranges::any_of(h.events.logs, [](const auto& entry) { return entry.first == LogLevel::Warning; });
    EXPECT_TRUE(warned);
}

TEST(BoundFlashAttemptTest, RequestUnblockReachesTheTransport)
{
    Harness h;
    h.transport->queueBlockingRead();

    h.attempt->request_unblock();

    auto read = h.transport->read(10, h.cancellation);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().kind, ErrorKind::Cancelled);
}

} // namespace
} // namespace fastecu::flash
