#include "src/backend/logging/logging_csv_columns.h"

#include <algorithm>
#include <iterator>
#include <ranges>

namespace fastecu::logging
{
Result<std::vector<LoggingCsvColumn>> PrepareLoggingCsvColumns(const LoggingRunSnapshot& snapshot)
{
    return snapshot.Measurements() |
           std::views::transform(
               [](const auto& measurement)
               { return LoggingCsvColumn{measurement.kind, measurement.identity, measurement.name}; }) |
           std::ranges::to<std::vector<LoggingCsvColumn>>();
}
} // namespace fastecu::logging
