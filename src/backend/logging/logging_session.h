#pragma once

#include <string_view>
#include <vector>

#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

class LoggingSession
{
  public:
    LoggingProtocolId Protocol() const;
    const std::vector<LoggingChannel>& Channels() const;
    const LoggingPolicy& Policy() const;
    const LoggingChannel *FindChannel(std::string_view id) const;

  private:
    LoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy);

    LoggingProtocolId protocol_;
    std::vector<LoggingChannel> channels_;
    LoggingPolicy policy_;

    friend fastecu::Result<LoggingSession>
    MakeLoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy);
};

// Validates individual channel fields. Aggregate wire shape and duplicate IDs
// remain the responsibility of MakeLoggingSession.
fastecu::Status ValidateLoggingChannel(LoggingProtocolId protocol, const LoggingChannel& channel);

fastecu::Result<LoggingSession> MakeLoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels,
                                                   LoggingPolicy policy);

} // namespace fastecu::logging
