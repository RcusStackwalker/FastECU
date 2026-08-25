#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{

bool valid_conversion_expression(std::string_view expression, std::uint8_t precision);
bool valid_display_precision(std::uint8_t precision);

class LoggingSession
{
  public:
    LoggingProtocolId protocol() const;
    const std::vector<LoggingChannel>& channels() const;
    const LoggingPolicy& policy() const;
    const LoggingChannel *find_channel(std::string_view id) const;

  private:
    LoggingSession(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy);

    LoggingProtocolId protocol_;
    std::vector<LoggingChannel> channels_;
    LoggingPolicy policy_;

    friend fastecu::Result<LoggingSession>
    make_logging_session(LoggingProtocolId protocol, std::vector<LoggingChannel> channels, LoggingPolicy policy);
};

fastecu::Result<LoggingSession> make_logging_session(LoggingProtocolId protocol, std::vector<LoggingChannel> channels,
                                                     LoggingPolicy policy);

} // namespace fastecu::logging
