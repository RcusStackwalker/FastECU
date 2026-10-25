#include "src/backend/flash/testing/execute_with_cancellation_guard.h"
#include "src/backend/flash/testing/guarded_attempt_expectations.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/backend/flash/flash_validation.h"
#include "src/backend/flash/guarded_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash::testing
{
namespace
{

Result<FlashPlan> ProbePlan()
{
    FlashPlanFields fields;
    fields.operation = FlashOperation::kRead;
    fields.family = FlashFamily::kDensoSh705xEepromCan;
    fields.transport = TransportKind::kCanIso15765;
    fields.target_id = "sub_ecu_denso_sh705x_eeprom_can";
    fields.mcu_name = "SH7058";
    fields.transfer_region = MemoryRegion{.start = 0, .length = 0x100};
    fields.kernel = KernelImage{.id = "k", .load_address = 0xffff6004, .bytes = {0x01, 0x02}};
    fields.family_plan = DensoSh705xEepromCanPlan{
        .mode = EepromReadMode::kMode2,
        .security = DensoSecurityVariant::kStock,
        .request_id = 0x7e0,
        .response_id = 0x7e8,
        .bitrate = 500000,
        .extended_id = false,
    };
    return ValidateAndBuild(std::move(fields));
}

class BlindTransport final : public ScriptedCanFlashTransport
{
  public:
    explicit BlindTransport(ManualCancellationToken& cancellation) : cancellation_(cancellation)
    {
        Exchange(bytes::Bytes{0x01}, bytes::Bytes{0x02});
    }
    Status Write(bytes::ByteView data, const ICancellationToken&) override
    {
        const auto result = ScriptedCanFlashTransport::Write(data, never_cancelled_);
        if (result.has_value())
        {
            cancellation_.Cancel();
        }
        return result;
    }
    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout, const ICancellationToken&) override
    {
        ++read_attempts;
        return ScriptedCanFlashTransport::Read(timeout, never_cancelled_);
    }
    int read_attempts = 0;

  private:
    ManualCancellationToken& cancellation_;
    ManualCancellationToken never_cancelled_;
};

class ProbeExecutor : public ICanFlashExecutor
{
  public:
    Result<Iso15765Config> TransportSetup(const FlashPlan&) const override
    {
        return Iso15765Config{.bitrate = 500000, .request_id = 0x7e0, .response_id = 0x7e8, .extended_id = false};
    }
    Result<FlashExecutionResult> Execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock&,
                                         const ICancellationToken& cancellation, IEventSink&) override
    {
        if (const auto written = transport.Write(bytes::Bytes{0x01}, cancellation); !written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (const auto read = transport.Read(std::chrono::milliseconds(10), cancellation); !read.has_value())
        {
            return std::unexpected(read.error());
        }
        return FlashExecutionResult{.operation = plan.Operation()};
    }
};

class GuardedProbeExecutor final : public ProbeExecutor
{
  public:
    using CancellationGuard = GuardedCanFlashTransport;
};

TEST(GuardedAttemptExpectations, DirectOptInRefusesReadAfterAcceptedWrite)
{
    const auto plan = ProbePlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ManualCancellationToken cancellation;
    BlindTransport transport(cancellation);
    FakeClock clock;
    RecordingEventSink events;
    GuardedProbeExecutor executor;

    EXPECT_THAT(ExecuteWithCancellationGuard(executor, *plan, transport, clock, cancellation, events),
                fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(transport.read_attempts, 0);
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_FALSE(transport.ScriptConsumed());
}

TEST(GuardedAttemptExpectations, DirectNonOptInKeepsItsPath)
{
    const auto plan = ProbePlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ManualCancellationToken cancellation;
    BlindTransport transport(cancellation);
    FakeClock clock;
    RecordingEventSink events;
    ProbeExecutor executor;

    EXPECT_THAT(ExecuteWithCancellationGuard(executor, *plan, transport, clock, cancellation, events),
                fastecu::testing::IsOk());
    EXPECT_EQ(transport.read_attempts, 1);
    EXPECT_EQ(transport.WritesConsumed(), 1U);
    EXPECT_TRUE(transport.ScriptConsumed());
}

TEST(GuardedAttemptExpectations, BoundAttemptUsesSelectedGuard)
{
    const auto plan = ProbePlan();
    ASSERT_THAT(plan, fastecu::testing::IsOk());
    ExpectCanGuardedAttempt<GuardedProbeExecutor>(*plan);
}

} // namespace
} // namespace fastecu::flash::testing
