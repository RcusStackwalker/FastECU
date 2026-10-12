#include "src/backend/logging/logging_csv_columns.h"

namespace fastecu::logging
{
Result<std::vector<LoggingCsvColumn>> PrepareLoggingCsvColumns(const LoggingRunSnapshot& snapshot)
{
    std::vector<LoggingCsvColumn> columns;
    columns.reserve(snapshot.Measurements().size());
    for (const auto& measurement : snapshot.Measurements())
    {
        columns.push_back({measurement.kind, measurement.identity, measurement.name});
    }
    return columns;
}
} // namespace fastecu::logging
