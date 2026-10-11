#pragma once

#include <map>
#include <QString>

#include "src/backend/logging/logger_model.h"
#include "src/backend/logging/logging_types.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"

namespace fastecu::desktop::logging
{
// Desktop display values; parameters and switches remain separate namespaces.
class DesktopLoggerValues
{
  public:
    void Initialize(const fastecu::logging::LoggerModel& model);
    QString ParameterValue(std::string_view protocol, std::string_view id) const;
    QString SwitchValue(std::string_view protocol, std::string_view id) const;
    bool SetParameterValue(const fastecu::logging::LoggerIdentity& identity, QString value);

  private:
    std::map<fastecu::logging::LoggerIdentity, QString> parameters_;
    std::map<fastecu::logging::LoggerIdentity, QString> switches_;
};
QString FormatLoggingValue(double value, int precision);
fastecu::Status ApplyLogSample(const DesktopLoggingSnapshot& snapshot, const fastecu::logging::LogSample& sample,
                               DesktopLoggerValues& values);
} // namespace fastecu::desktop::logging
