#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include <QFile>
#include <QString>

#include "src/backend/logging/logging_run_snapshot.h"
#include "src/backend/logging/logging_csv_columns.h"
#include "src/backend/ports/result.h"
#include "src/platform/desktop/common/logging/logging_value_adapter.h"

namespace fastecu::desktop::logging
{
class LoggingCsvFile
{
  public:
    explicit LoggingCsvFile(std::unique_ptr<QFile> file = std::make_unique<QFile>());
    Result<QString> BeginRun(const fastecu::logging::LoggingRunSnapshot& snapshot, const QString& directory,
                             const QString& name_stem);
    Status AppendRow(const DesktopLoggerValues& values, std::chrono::milliseconds elapsed);
    Status EndRun();
    bool IsOpen() const;

  private:
    Status WriteRecord(const std::vector<std::string>& fields);
    std::unique_ptr<QFile> file_;
    std::vector<fastecu::logging::LoggingCsvColumn> columns_;
};
} // namespace fastecu::desktop::logging
