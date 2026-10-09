#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "src/backend/logging/logger_model.h"
#include "src/backend/logging/logging_session.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{
enum class LoggingTarget
{
    kEcu,
    kTcu,
};

class LoggingRunSnapshot
{
  public:
    const LoggingSession& Session() const;
    const std::string& ProtocolKey() const;
    const LoggerSelection& Selection() const;
    const std::vector<std::size_t>& ResponseOffsets() const;
    LoggingTarget Target() const;
    bool ChannelEnabled(std::string_view id) const;

  private:
    LoggingRunSnapshot(LoggingSession session, std::string protocol_key, LoggerSelection selection,
                       std::vector<std::size_t> response_offsets, std::unordered_set<std::string> enabled_ids,
                       LoggingTarget target);

    LoggingSession session_;
    std::string protocol_key_;
    LoggerSelection selection_;
    std::vector<std::size_t> response_offsets_;
    std::unordered_set<std::string> enabled_ids_;
    LoggingTarget target_;

    friend fastecu::Result<LoggingRunSnapshot> PrepareLoggingRun(const LoggerModel& model, LoggingProtocolId protocol,
                                                                 std::string_view protocol_filter, LoggingPolicy policy,
                                                                 LoggingTarget target);
};

fastecu::Result<LoggingRunSnapshot> PrepareLoggingRun(const LoggerModel& model, LoggingProtocolId protocol,
                                                      std::string_view protocol_filter, LoggingPolicy policy,
                                                      LoggingTarget target);
} // namespace fastecu::logging
