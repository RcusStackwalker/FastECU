#include "src/backend/ports/testing/result_matchers.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_types.h"
#include "src/backend/flash/flash_executor.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <concepts>
#include <string>
#include <vector>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/guarded_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"
#include "src/backend/ports/testing/fake_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

using ::testing::_;
using ::testing::Eq;
using ::testing::Optional;

namespace fastecu::flash
{
namespace
{

FlashPlanFields KlineReadFields()
{
    return FlashPlanFields{
        .operation = FlashOperation::kRead,
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

Result<FlashPlan> BuiltPlan()
{
    return ValidateAndBuild(KlineReadFields());
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

    explicit Harness(FlashPlan plan)
    {
        auto owned_transport = std::make_unique<ScriptedKlineFlashTransport>();
        auto owned_executor = std::make_unique<FakeKlineExecutor>();
        transport = owned_transport.get();
        executor = owned_executor.get();
        attempt = BindFlashAttempt(std::move(plan), std::move(owned_executor), std::move(owned_transport));
    }

    Result<FlashExecutionResult> Run()
    {
        return attempt->Run(clock, cancellation, events);
    }
};

TEST(BoundFlashAttemptTest, ConfiguresAndOpensBeforeExecuteAndClosesOnce)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));

    ASSERT_THAT(h.Run(), fastecu::testing::IsOk());
    ASSERT_THAT(h.transport->last_config, Optional(_));
    EXPECT_EQ(h.transport->last_config->baud, kSetup.baud);
    EXPECT_EQ(h.transport->last_config->tester_id, kSetup.tester_id);
    EXPECT_TRUE(h.executor->saw_open_transport);
    EXPECT_EQ(h.transport->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, InvalidPlanTouchesNoTransportCall)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
    h.executor->setup_ok = false;

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kInvalidConfig));
    EXPECT_THAT(h.transport->last_config, Eq(std::nullopt));
    EXPECT_EQ(h.transport->close_call_count, 0);
    EXPECT_EQ(h.executor->execute_calls, 0);
}

TEST(BoundFlashAttemptTest, ConfigureFailureSkipsOpenAndExecuteAndClose)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
    h.transport->configure_result = Fail(ErrorKind::kDisconnected, "no port");

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count, 0);
}

TEST(BoundFlashAttemptTest, OpenFailureSkipsExecuteAndClose)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
    h.transport->open_result = Fail(ErrorKind::kDisconnected, "open failed");

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_EQ(h.executor->execute_calls, 0);
    EXPECT_EQ(h.transport->close_call_count, 0);
}

TEST(BoundFlashAttemptTest, CancelledBeforeConfigureDoesNotConfigure)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
    h.cancellation.SetCancelled(true);

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_THAT(h.transport->last_config, Eq(std::nullopt));
}

TEST(BoundFlashAttemptTest, CancellationIsNotUniversallyPolledBetweenConfigureAndOpen)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
    h.cancellation.CancelOnCheck(2);

    ASSERT_THAT(h.Run(), fastecu::testing::IsOk());
    EXPECT_THAT(h.transport->last_config, Optional(_));
    EXPECT_EQ(h.executor->execute_calls, 1);
    EXPECT_EQ(h.transport->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorIsReturnedAndTransportStillClosesOnce)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
    h.executor->execute_ok = false;

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kBadResponse));
    EXPECT_EQ(h.transport->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, CloseOnlyErrorIsReturned)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
    h.transport->close_result = Fail(ErrorKind::kInternal, "close failed");

    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_EQ(h.transport->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, ExecuteErrorWinsOverCloseErrorAndCloseIsLogged)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
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

    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    Harness h(std::move(*built_plan));
    h.transport->QueueBlockingRead();

    h.attempt->RequestUnblock();

    auto read = h.transport->Read(10ms, h.cancellation);
    ASSERT_THAT(read, fastecu::testing::IsErr(ErrorKind::kCancelled));
}

TEST(BoundFlashAttemptTest, MixedTransportUsesOuterLifecycleExactlyOnce)
{
    std::vector<std::string> calls;
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    auto attempt = BindFlashAttempt(std::move(*built_plan), std::make_unique<TestMixedExecutor>(calls),
                                    std::make_unique<RecordingMixedTransport>(calls));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(attempt->Run(clock, cancellation, events), fastecu::testing::IsOk());
    EXPECT_THAT(calls, ::testing::ElementsAre("before_transport_configure", "configure", "open", "execute", "close"));
}

TEST(BoundFlashAttemptTest, MixedTransportBeforeConfigureErrorSkipsConfigureOpenExecuteClose)
{
    std::vector<std::string> calls;
    auto executor = std::make_unique<TestMixedExecutor>(calls);
    executor->before_configure_ok = false;
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    auto attempt =
        BindFlashAttempt(std::move(*built_plan), std::move(executor), std::make_unique<RecordingMixedTransport>(calls));
    FakeClock clock;
    FakeCancellationToken cancellation;
    RecordingEventSink events;

    ASSERT_THAT(attempt->Run(clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kInternal));
    EXPECT_THAT(calls, ::testing::ElementsAre("before_transport_configure"));
    EXPECT_THAT(calls, ::testing::Not(::testing::Contains("configure")));
}

// Deliberately cancellation-blind: only the runner's opted-in guard can refuse.
class BlindCanTransport final : public ScriptedCanFlashTransport
{
  public:
    Status Write(bytes::ByteView, const ICancellationToken&) override
    {
        ++write_calls;
        return {};
    }
    void RequestUnblock() noexcept override
    {
        ++unblock_calls;
    }
    int write_calls = 0;
    int unblock_calls = 0;
};

class CanExecutorProbe : public ICanFlashExecutor
{
  public:
    explicit CanExecutorProbe(ManualCancellationToken *cancel_inside) : cancel_inside_(cancel_inside)
    {
    }
    Result<Iso15765Config> TransportSetup(const FlashPlan&) const override
    {
        return Iso15765Config{500000, 0x7E0, 0x7E8, false};
    }
    Result<FlashExecutionResult> Execute(const FlashPlan&, ICanFlashTransport& transport, IClock&,
                                         const ICancellationToken& token, IEventSink&) override
    {
        if (cancel_inside_ != nullptr)
        {
            cancel_inside_->Cancel();
        }
        if (auto result = transport.Write(bytes::Bytes{0x12}, token); !result.has_value())
        {
            return std::unexpected(result.error());
        }
        return FlashExecutionResult{.operation = FlashOperation::kRead};
    }

  private:
    ManualCancellationToken *cancel_inside_;
};

class GuardedCanExecutorProbe : public CanExecutorProbe
{
  public:
    using CancellationGuard = GuardedCanFlashTransport;
    using CanExecutorProbe::CanExecutorProbe;
};

template <typename Executor = GuardedCanExecutorProbe> struct CanProbeHarness
{
    explicit CanProbeHarness(FlashPlan plan, bool cancel_inside = true)
    {
        auto owned = std::make_unique<BlindCanTransport>();
        inner = owned.get();
        attempt = BindFlashAttempt(std::move(plan), std::make_unique<Executor>(cancel_inside ? &token : nullptr),
                                   std::move(owned));
    }
    Result<FlashExecutionResult> Run()
    {
        return attempt->Run(clock, token, events);
    }
    ManualCancellationToken token;
    FakeClock clock;
    RecordingEventSink events;
    BlindCanTransport *inner = nullptr;
    std::unique_ptr<BoundFlashAttempt> attempt;
};

TEST(BoundFlashAttemptTest, OptedInExecutorRefusesCancelledWrite)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    CanProbeHarness h(std::move(*built_plan));
    const auto result = h.Run();
    EXPECT_EQ(h.inner->write_calls, 0);
    EXPECT_EQ(h.inner->configure_call_count, 1);
    EXPECT_EQ(h.inner->open_call_count, 1);
    EXPECT_EQ(h.inner->close_call_count, 1);
    ASSERT_THAT(result, fastecu::testing::IsErr(ErrorKind::kCancelled));
}

TEST(BoundFlashAttemptTest, UnoptedInCanExecutorKeepsItsExistingPath)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    CanProbeHarness<CanExecutorProbe> h(std::move(*built_plan));
    ASSERT_THAT(h.Run(), fastecu::testing::IsOk());
    EXPECT_EQ(h.inner->write_calls, 1);
    EXPECT_EQ(h.inner->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, OptedInExecutionErrorWinsOverCloseError)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    CanProbeHarness h(std::move(*built_plan));
    h.inner->close_result = Fail(ErrorKind::kInternal, "close failed");
    ASSERT_THAT(h.Run(), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(h.inner->close_call_count, 1);
    EXPECT_THAT(h.events.logs,
                ::testing::Contains(::testing::Pair(LogLevel::kWarning, "close failed after execution error")));
}

TEST(BoundFlashAttemptTest, OptedInCloseOnlyFailureIsReturned)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    CanProbeHarness h(std::move(*built_plan), false);
    h.inner->close_result = Fail(ErrorKind::kInternal, "close failed");
    ASSERT_THAT(h.Run(), fastecu::testing::IsErrWith(ErrorKind::kInternal, "close failed"));
    EXPECT_EQ(h.inner->close_call_count, 1);
}

TEST(BoundFlashAttemptTest, OptedInUnblockReachesOwnedTransport)
{
    auto built_plan = BuiltPlan();
    ASSERT_THAT(built_plan, fastecu::testing::IsOk());
    CanProbeHarness h(std::move(*built_plan), false);
    h.attempt->RequestUnblock();
    EXPECT_EQ(h.inner->unblock_calls, 1);
    ASSERT_THAT(h.Run(), fastecu::testing::IsOk());
    h.attempt->RequestUnblock();
    EXPECT_EQ(h.inner->unblock_calls, 2);
}

} // namespace
} // namespace fastecu::flash
