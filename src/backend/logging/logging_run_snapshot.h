#pragma once

#include <cstdint>
#include <string>
#include <string_view>
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

enum class LoggingMeasurementKind
{
    kParameter,
    kSwitch
};
struct LoggingMeasurement
{
    LoggingMeasurementKind kind;
    LoggerIdentity identity;
    std::string channel_id;
    std::string name;
    std::string unit;
    std::uint8_t decimal_precision;
    EcuSupport support;
};

class LoggingRunSnapshot
{
  public:
    const LoggingSession& Session() const;
    const std::vector<LoggingMeasurement>& Measurements() const;
    const LoggingMeasurement *FindMeasurement(LoggingMeasurementKind kind, std::string_view id) const;
    const std::string& ProtocolKey() const;
    const LoggerSelection& Selection() const;
    LoggingTarget Target() const;

  private:
    LoggingRunSnapshot(LoggingSession session, std::string protocol_key, LoggerSelection selection,
                       std::vector<LoggingMeasurement> measurements, LoggingTarget target);

    LoggingSession session_;
    std::string protocol_key_;
    LoggerSelection selection_;
    LoggingTarget target_;
    std::vector<LoggingMeasurement> measurements_;

    friend fastecu::Result<LoggingRunSnapshot> PrepareLoggingRun(const LoggerModel& model, LoggingProtocolId protocol,
                                                                 std::string_view protocol_filter, LoggingPolicy policy,
                                                                 LoggingTarget target);
};

fastecu::Result<LoggingRunSnapshot> PrepareLoggingRun(const LoggerModel& model, LoggingProtocolId protocol,
                                                      std::string_view protocol_filter, LoggingPolicy policy,
                                                      LoggingTarget target);
} // namespace fastecu::logging
