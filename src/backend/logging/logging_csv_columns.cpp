#include "src/backend/logging/logging_csv_columns.h"

#include <algorithm>
#include <iterator>

namespace fastecu::logging
{
Result<std::vector<LoggingCsvColumn>> PrepareLoggingCsvColumns(const LoggingRunSnapshot& snapshot)
{
    std::vector<LoggingCsvColumn> columns;
    columns.reserve(snapshot.Measurements().size());
    std::ranges::transform(snapshot.Measurements(), std::back_inserter(columns), [](const auto& measurement)
                           { return LoggingCsvColumn{measurement.kind, measurement.identity, measurement.name}; });
    return columns;
}
} // namespace fastecu::logging
