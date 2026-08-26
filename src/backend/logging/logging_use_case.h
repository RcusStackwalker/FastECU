#pragma once

#include "src/backend/logging/logging_event_sink.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/logging/logging_protocol.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

namespace detail
{
bool record_silent_miss(int& consecutive_misses, int threshold);
}

class LoggingUseCase
{
  public:
    explicit LoggingUseCase(fastecu::IClock& clock) : clock_(clock)
    {
    }

    fastecu::Status run(const LoggingSession& session, LoggingProtocol& protocol,
                        const fastecu::ICancellationToken& cancellation, ILoggingEventSink& events,
                        fastecu::IEventSink& diagnostics) const;

  private:
    fastecu::IClock& clock_;
};

} // namespace fastecu::logging
