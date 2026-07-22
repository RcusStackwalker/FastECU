#include "src/backend/logging/logging_use_case.h"

#include <string_view>
#include <utility>
#include <vector>

#include "src/backend/logging/logging_conversion.h"

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
        const fastecu::Status stop_result = protocol_.stop();
        if (!stop_result)
        {
            diagnostics_.log(fastecu::LogLevel::Error, stop_result.error().detail);
        }
    }

  private:
    LoggingProtocol& protocol_;
    fastecu::IEventSink& diagnostics_;
};

bool reconnect_due(const LoggingPolicy& policy, int consecutive_misses)
{
    return policy.reconnect_retry_period > 0 &&
           consecutive_misses >= policy.reconnect_attempt_threshold &&
           (consecutive_misses - policy.reconnect_attempt_threshold) %
                   policy.reconnect_retry_period ==
               0;
}

} // namespace

fastecu::Status LoggingUseCase::run(const LoggingSession& session, LoggingProtocol& protocol,
                                    const fastecu::ICancellationToken& cancellation,
                                    ILoggingEventSink& events,
                                    fastecu::IEventSink& diagnostics) const
{
    if (cancellation.cancelled())
    {
        return fastecu::fail(fastecu::ErrorKind::Cancelled, "logging cancelled");
    }

    StopGuard stop_guard(protocol, diagnostics);
    if (const fastecu::Status started = protocol.start(cancellation); !started)
    {
        return std::unexpected(started.error());
    }

    events.state_changed(LoggingState::Running);
    LoggingState last_state = LoggingState::Running;
    int consecutive_misses = 0;

    while (!cancellation.cancelled())
    {
        auto poll_result = protocol.poll(session.policy().poll_timeout_ms, cancellation);
        if (!poll_result)
        {
            if (poll_result.error().kind == fastecu::ErrorKind::BadResponse)
            {
                continue;
            }
            return std::unexpected(poll_result.error());
        }

        if (poll_result->responded)
        {
            consecutive_misses = 0;
            if (last_state != LoggingState::Running)
            {
                last_state = LoggingState::Running;
                events.state_changed(LoggingState::Running);
            }

            std::vector<LogSample> converted;
            converted.reserve(poll_result->samples.size());
            for (const ProtocolSample& raw : poll_result->samples)
            {
                auto sample = convert_sample(session, raw);
                if (!sample)
                {
                    return std::unexpected(sample.error());
                }
                converted.push_back(std::move(*sample));
            }
            events.samples(converted);
            continue;
        }

        ++consecutive_misses;
        if (consecutive_misses == session.policy().car_silence_miss_threshold)
        {
            last_state = LoggingState::CarNotResponding;
            events.state_changed(LoggingState::CarNotResponding);
        }

        if (!reconnect_due(session.policy(), consecutive_misses))
        {
            continue;
        }

        const fastecu::Status reconnected = protocol.start(cancellation);
        if (!reconnected)
        {
            if (reconnected.error().kind != fastecu::ErrorKind::BadResponse)
            {
                return std::unexpected(reconnected.error());
            }
            continue;
        }

        consecutive_misses = 0;
        last_state = LoggingState::Running;
        events.state_changed(LoggingState::Running);
    }

    return fastecu::fail(fastecu::ErrorKind::Cancelled, "logging cancelled");
}
