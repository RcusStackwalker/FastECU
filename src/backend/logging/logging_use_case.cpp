#include "src/backend/logging/logging_use_case.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
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

struct MonotonicDeadline
{
    std::uint64_t at_ms;
    bool reachable;
};

MonotonicDeadline saturated_deadline(std::uint64_t base, std::uint64_t delta)
{
    const auto max = std::numeric_limits<std::uint64_t>::max();
    if (delta > max - base)
    {
        return {.at_ms = max, .reachable = false};
    }
    return {.at_ms = base + delta, .reachable = true};
}

} // namespace

fastecu::Status LoggingUseCase::run(const LoggingSession& session, LoggingProtocol& protocol,
                                    const fastecu::ICancellationToken& cancellation, ILoggingEventSink& events,
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
    std::uint32_t reconnect_attempts = 0;
    std::optional<MonotonicDeadline> reconnect_deadline;
    std::string last_retry_failure_detail;

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
            if (poll_result->samples.empty())
            {
                continue;
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

            consecutive_misses = 0;
            reconnect_attempts = 0;
            reconnect_deadline.reset();
            last_retry_failure_detail.clear();
            if (last_state != LoggingState::Running)
            {
                last_state = LoggingState::Running;
                events.state_changed(LoggingState::Running);
            }
            events.samples(converted);
            continue;
        }

        ++consecutive_misses;
        if (consecutive_misses == session.policy().car_silence_miss_threshold)
        {
            last_state = LoggingState::CarNotResponding;
            events.state_changed(LoggingState::CarNotResponding);
            reconnect_deadline = saturated_deadline(
                clock_.now_ms(), static_cast<std::uint64_t>(session.policy().reconnect_initial_delay_ms));
        }

        if (!reconnect_deadline || !reconnect_deadline->reachable || clock_.now_ms() < reconnect_deadline->at_ms)
        {
            continue;
        }

        if (session.policy().max_reconnect_attempts && reconnect_attempts >= *session.policy().max_reconnect_attempts)
        {
            if (!last_retry_failure_detail.empty())
            {
                diagnostics.log(fastecu::LogLevel::Error, last_retry_failure_detail);
            }
            return fastecu::fail(fastecu::ErrorKind::BadResponse, "logging reconnect attempts exhausted");
        }

        const fastecu::Status reconnected = protocol.start(cancellation);
        ++reconnect_attempts;
        reconnect_deadline =
            saturated_deadline(clock_.now_ms(), static_cast<std::uint64_t>(session.policy().reconnect_period_ms));
        if (!reconnected)
        {
            if (reconnected.error().kind != fastecu::ErrorKind::BadResponse)
            {
                return std::unexpected(reconnected.error());
            }
            last_retry_failure_detail = reconnected.error().detail;
            continue;
        }
    }

    return fastecu::fail(fastecu::ErrorKind::Cancelled, "logging cancelled");
}

} // namespace fastecu::logging
