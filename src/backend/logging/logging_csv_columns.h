#pragma once

#include <string>
#include <vector>

#include "src/backend/logging/logging_run_snapshot.h"
#include "src/backend/logging/logger_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::logging
{
struct LoggingCsvColumn
{
    LoggingMeasurementKind kind;
    LoggerIdentity identity;
    std::string name;
};
Result<std::vector<LoggingCsvColumn>> prepare_logging_csv_columns(const LoggingRunSnapshot& snapshot);
} // namespace fastecu::logging
