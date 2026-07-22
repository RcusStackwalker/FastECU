#pragma once

#include "src/backend/logging/logging_event_sink.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/logging/portable_logging_protocol.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

class LoggingUseCase
{
  public:
    fastecu::Status run(const LoggingSession& session, LoggingProtocol& protocol,
                        const fastecu::ICancellationToken& cancellation,
                        ILoggingEventSink& events, fastecu::IEventSink& diagnostics) const;
};

} // namespace fastecu::logging
