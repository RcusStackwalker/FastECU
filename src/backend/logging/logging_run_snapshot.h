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
    Ecu,
    Tcu,
};

class LoggingRunSnapshot
{
  public:
    const LoggingSession& session() const;
    const std::string& protocol_key() const;
    const LoggerSelection& selection() const;
    const std::vector<std::size_t>& response_offsets() const;
    LoggingTarget target() const;
    bool channel_enabled(std::string_view id) const;

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

    friend fastecu::Result<LoggingRunSnapshot> prepare_logging_run(const LoggerModel& model, LoggingProtocolId protocol,
                                                                   std::string_view protocol_filter,
                                                                   LoggingPolicy policy, LoggingTarget target);
};

fastecu::Result<LoggingRunSnapshot> prepare_logging_run(const LoggerModel& model, LoggingProtocolId protocol,
                                                        std::string_view protocol_filter, LoggingPolicy policy,
                                                        LoggingTarget target);
} // namespace fastecu::logging
