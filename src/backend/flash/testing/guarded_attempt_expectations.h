#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/flash/testing/scripted_mixed_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"
#include "src/backend/ports/testing/result_matchers.h"

namespace fastecu::flash::testing
{
namespace guarded_attempt_detail
{

// Count attempts before delegation; a scripted transport's own token checks
// must not conceal a missing guard in the runner.
class BlindCanTransport final : public ScriptedCanFlashTransport
{
  public:
    explicit BlindCanTransport(ManualCancellationToken& cancellation) : cancellation_(cancellation)
    {
        Exchange(bytes::Bytes{0x01}, bytes::Bytes{0x02});
    }
    Status Write(bytes::ByteView data, const ICancellationToken&) override
    {
        ++write_attempts;
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
    int write_attempts = 0;
    int read_attempts = 0;

  private:
    ManualCancellationToken& cancellation_;
    ManualCancellationToken never_cancelled_;
};

class CanProbeExecutor : public ICanFlashExecutor
{
  public:
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

template <typename Executor> class CanProbe final : public CanProbeExecutor
{
  public:
    using CancellationGuard = typename Executor::CancellationGuard;

    Result<Iso15765Config> TransportSetup(const FlashPlan& plan) const override
    {
        return Executor{}.TransportSetup(plan);
    }
};

class BlindMixedTransport final : public IMixedCanFlashTransport
{
  public:
    explicit BlindMixedTransport(ManualCancellationToken& cancellation, bool raw) : cancellation_(cancellation)
    {
        if (raw)
        {
            script.ExpectRawWrite(cdbg::CanFrame{.id = 0x7e0, .payload = {0x01}});
            script.QueueRawRead(cdbg::CanFrame{.id = 0x7e8, .payload = {0x02}});
        }
        else
        {
            script.ExpectIsoWrite(bytes::Bytes{0x01});
            script.QueueIsoRead(bytes::Bytes{0x02});
        }
    }
    Status ResetConnection() override
    {
        return script.ResetConnection();
    }
    Status Configure(const MixedCanConfig& config) override
    {
        return script.Configure(config);
    }
    Status Open() override
    {
        return script.Open();
    }
    Status Close() override
    {
        return script.Close();
    }
    Status EnterRawBootloaderMode() override
    {
        return script.EnterRawBootloaderMode();
    }
    Status ClearReceiveBuffer() override
    {
        return script.ClearReceiveBuffer();
    }
    Status EnterIso15765KernelMode() override
    {
        return script.EnterIso15765KernelMode();
    }
    void RequestUnblock() noexcept override
    {
        script.RequestUnblock();
    }
    Status WriteRaw(const cdbg::CanFrame& frame, const ICancellationToken&) override
    {
        ++write_attempts;
        return CancelAfterAccepted(script.WriteRaw(frame, never_cancelled_));
    }
    Status WriteIso15765(bytes::ByteView data, const ICancellationToken&) override
    {
        ++write_attempts;
        return CancelAfterAccepted(script.WriteIso15765(data, never_cancelled_));
    }
    Result<std::optional<cdbg::CanFrame>> ReadRaw(std::chrono::milliseconds timeout, const ICancellationToken&) override
    {
        ++read_attempts;
        return script.ReadRaw(timeout, never_cancelled_);
    }
    Result<std::optional<bytes::Bytes>> ReadIso15765(std::chrono::milliseconds timeout,
                                                     const ICancellationToken&) override
    {
        ++read_attempts;
        return script.ReadIso15765(timeout, never_cancelled_);
    }
    ScriptedMixedCanFlashTransport script;
    int write_attempts = 0;
    int read_attempts = 0;
    int accepted_writes = 0;

  private:
    Status CancelAfterAccepted(Status result)
    {
        if (result.has_value())
        {
            ++accepted_writes;
            cancellation_.Cancel();
        }
        return result;
    }
    ManualCancellationToken& cancellation_;
    ManualCancellationToken never_cancelled_;
};

template <typename Executor> class MixedProbe final : public IMixedCanFlashExecutor
{
  public:
    using CancellationGuard = typename Executor::CancellationGuard;
    explicit MixedProbe(bool raw) : raw_(raw)
    {
    }
    Result<MixedCanConfig> TransportSetup(const FlashPlan& plan) const override
    {
        return Executor{}.TransportSetup(plan);
    }
    Result<FlashExecutionResult> Execute(const FlashPlan& plan, IMixedCanFlashTransport& transport, IClock&,
                                         const ICancellationToken& cancellation, IEventSink&) override
    {
        if (const auto raw_mode = transport.EnterRawBootloaderMode(); !raw_mode.has_value())
        {
            return std::unexpected(raw_mode.error());
        }
        if (!raw_)
        {
            if (const auto iso_mode = transport.EnterIso15765KernelMode(); !iso_mode.has_value())
            {
                return std::unexpected(iso_mode.error());
            }
        }
        const auto written = raw_ ? transport.WriteRaw(cdbg::CanFrame{.id = 0x7e0, .payload = {0x01}}, cancellation)
                                  : transport.WriteIso15765(bytes::Bytes{0x01}, cancellation);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (raw_)
        {
            if (const auto read = transport.ReadRaw(std::chrono::milliseconds(10), cancellation); !read.has_value())
            {
                return std::unexpected(read.error());
            }
        }
        else if (const auto read = transport.ReadIso15765(std::chrono::milliseconds(10), cancellation);
                 !read.has_value())
        {
            return std::unexpected(read.error());
        }
        return FlashExecutionResult{.operation = plan.Operation()};
    }

  private:
    bool raw_;
};

} // namespace guarded_attempt_detail

// This is a synthetic selection/wiring proof, not a family's protocol run.
template <typename Executor> void ExpectCanGuardedAttempt(const FlashPlan& plan)
{
    ManualCancellationToken cancellation;
    auto transport = std::make_unique<guarded_attempt_detail::BlindCanTransport>(cancellation);
    const auto *inner = transport.get();
    auto attempt =
        BindFlashAttempt(plan, std::make_unique<guarded_attempt_detail::CanProbe<Executor>>(), std::move(transport));
    FakeClock clock;
    RecordingEventSink events;
    EXPECT_THAT(attempt->Run(clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kCancelled));
    EXPECT_EQ(inner->write_attempts, 1);
    EXPECT_EQ(inner->WritesConsumed(), 1U);
    EXPECT_EQ(inner->read_attempts, 0);
    EXPECT_FALSE(inner->ScriptConsumed());
    EXPECT_EQ(inner->configure_call_count, 1);
    EXPECT_EQ(inner->open_call_count, 1);
    EXPECT_EQ(inner->close_call_count, 1);
}

template <typename Executor> void ExpectMixedCanGuardedAttempt(const FlashPlan& plan)
{
    for (const bool raw : {true, false})
    {
        SCOPED_TRACE(raw ? "raw CAN" : "ISO-15765");
        ManualCancellationToken cancellation;
        auto transport = std::make_unique<guarded_attempt_detail::BlindMixedTransport>(cancellation, raw);
        const auto *inner = transport.get();
        auto attempt = BindFlashAttempt(plan, std::make_unique<guarded_attempt_detail::MixedProbe<Executor>>(raw),
                                        std::move(transport));
        FakeClock clock;
        RecordingEventSink events;
        EXPECT_THAT(attempt->Run(clock, cancellation, events), fastecu::testing::IsErr(ErrorKind::kCancelled));
        EXPECT_EQ(inner->write_attempts, 1);
        EXPECT_EQ(inner->accepted_writes, 1);
        EXPECT_EQ(inner->read_attempts, 0);
        EXPECT_FALSE(inner->script.ScriptConsumed());
        EXPECT_EQ(inner->script.ConfigureCallCount(), 1);
        EXPECT_EQ(inner->script.OpenCallCount(), 1);
        EXPECT_EQ(inner->script.CloseCallCount(), 1);
        const auto& modes = inner->script.ModeChanges();
        if (raw)
        {
            EXPECT_THAT(modes,
                        ::testing::ElementsAre(ScriptedMixedCanMode::kIso15765Kernel,
                                               ScriptedMixedCanMode::kRawBootloader, ScriptedMixedCanMode::kClosed));
        }
        else
        {
            EXPECT_THAT(modes, ::testing::ElementsAre(
                                   ScriptedMixedCanMode::kIso15765Kernel, ScriptedMixedCanMode::kRawBootloader,
                                   ScriptedMixedCanMode::kIso15765Kernel, ScriptedMixedCanMode::kClosed));
        }
    }
}

} // namespace fastecu::flash::testing
