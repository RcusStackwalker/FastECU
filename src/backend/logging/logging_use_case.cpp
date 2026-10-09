#include "src/backend/logging/logging_use_case.h"

#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/logging/logging_conversion.h"

namespace fastecu::logging
{

namespace
{

class StopGuard
{
  public:
    StopGuard(LoggingProtocol& protocol, fastecu::IEventSink& diagnostics)
        : protocol_(protocol), diagnostics_(diagnostics)
    {
    }

    ~StopGuard()
    {
        const fastecu::Status stop_result = protocol_.Stop();
        if (!stop_result)
        {
            diagnostics_.Log(fastecu::LogLevel::kError, stop_result.error().detail);
        }
    }

  private:
    LoggingProtocol& protocol_;
    fastecu::IEventSink& diagnostics_;
};

bool ReconnectDue(const LoggingPolicy& policy, int consecutive_misses)
{
    return policy.reconnect_retry_period > 0 && consecutive_misses >= policy.reconnect_attempt_threshold &&
           (consecutive_misses - policy.reconnect_attempt_threshold) % policy.reconnect_retry_period == 0;
}

} // namespace

fastecu::Status LoggingUseCase::Run(const LoggingSession& session, LoggingProtocol& protocol,
                                    const fastecu::ICancellationToken& cancellation, ILoggingEventSink& events,
                                    fastecu::IEventSink& diagnostics) const
{
    if (cancellation.Cancelled())
    {
        return fastecu::Fail(fastecu::ErrorKind::kCancelled, "logging cancelled");
    }

    StopGuard stop_guard(protocol, diagnostics);
    if (const fastecu::Status started = protocol.Start(cancellation); !started)
    {
        return std::unexpected(started.error());
    }

    events.StateChanged(LoggingState::kRunning);
    LoggingState last_state = LoggingState::kRunning;
    int consecutive_misses = 0;

    while (!cancellation.Cancelled())
    {
        auto poll_result = protocol.Poll(session.Policy().poll_timeout, cancellation);
        if (!poll_result)
        {
            if (poll_result.error().kind == fastecu::ErrorKind::kBadResponse)
            {
                continue;
            }
            return std::unexpected(poll_result.error());
        }

        if (poll_result->responded)
        {
            consecutive_misses = 0;
            if (last_state != LoggingState::kRunning)
            {
                last_state = LoggingState::kRunning;
                events.StateChanged(LoggingState::kRunning);
            }

            std::vector<LogSample> converted;
            converted.reserve(poll_result->samples.size());
            for (const ProtocolSample& raw : poll_result->samples)
            {
                auto sample = ConvertSample(session, raw);
                if (!sample)
                {
                    return std::unexpected(sample.error());
                }
                converted.push_back(std::move(*sample));
            }
            events.Samples(converted);
            continue;
        }

        ++consecutive_misses;
        if (consecutive_misses == session.Policy().car_silence_miss_threshold)
        {
            last_state = LoggingState::kCarNotResponding;
            events.StateChanged(LoggingState::kCarNotResponding);
        }

        if (!ReconnectDue(session.Policy(), consecutive_misses))
        {
            continue;
        }

        if (const fastecu::Status reconnected = protocol.Start(cancellation); !reconnected)
        {
            if (reconnected.error().kind != fastecu::ErrorKind::kBadResponse)
            {
                return std::unexpected(reconnected.error());
            }
            continue;
        }

        consecutive_misses = 0;
        last_state = LoggingState::kRunning;
        events.StateChanged(LoggingState::kRunning);
    }

    return fastecu::Fail(fastecu::ErrorKind::kCancelled, "logging cancelled");
}

} // namespace fastecu::logging
