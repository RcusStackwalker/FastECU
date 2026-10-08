#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <QFile>
#include <QString>

#include "src/backend/logging/logging_run_snapshot.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"

namespace fastecu::desktop::logging
{
class LoggingCsvFile
{
  public:
    explicit LoggingCsvFile(std::unique_ptr<QFile> file = std::make_unique<QFile>());
    Result<QString> begin_run(const fastecu::logging::LoggingRunSnapshot& snapshot, const QString& directory,
                              const QString& name_stem);
    Status append_row(const DesktopLoggerValues& values, std::chrono::milliseconds elapsed);
    Status end_run();
    bool is_open() const;

  private:
    struct Column
    {
        fastecu::logging::LoggingMeasurementKind kind;
        fastecu::logging::LoggerIdentity identity;
        std::string name;
    };
    Status write_record(const std::vector<std::string>& fields);
    std::unique_ptr<QFile> file_;
    std::vector<Column> columns_;
};
} // namespace fastecu::desktop::logging
