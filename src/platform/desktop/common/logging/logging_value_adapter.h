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
    void initialize(const fastecu::logging::LoggerModel& model);
    QString parameter_value(std::string_view protocol, std::string_view id) const;
    QString switch_value(std::string_view protocol, std::string_view id) const;
    bool set_parameter_value(const fastecu::logging::LoggerIdentity& identity, QString value);

  private:
    std::map<fastecu::logging::LoggerIdentity, QString> parameters_;
    std::map<fastecu::logging::LoggerIdentity, QString> switches_;
};
QString format_logging_value(double value, int precision);
fastecu::Status apply_log_sample(const DesktopLoggingSnapshot& snapshot, const fastecu::logging::LogSample& sample,
                                 DesktopLoggerValues& values);
} // namespace fastecu::desktop::logging
