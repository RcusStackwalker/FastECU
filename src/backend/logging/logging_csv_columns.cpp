#include "src/backend/logging/logging_csv_columns.h"

namespace fastecu::logging
{
Result<std::vector<LoggingCsvColumn>> PrepareLoggingCsvColumns(const LoggingRunSnapshot& snapshot)
{
    std::vector<LoggingCsvColumn> columns;
    const auto Add = [&](LoggingMeasurementKind kind, const auto& ids) -> Status
    {
        for (const auto& id : ids)
        {
            const auto *measurement = snapshot.FindMeasurement(kind, id);
            if (measurement == nullptr)
            {
                return Fail(ErrorKind::kInternal, "CSV selection has no captured measurement");
            }
            columns.push_back({kind, measurement->identity, measurement->name});
        }
        return {};
    };
    for (const auto& ids : {snapshot.Selection().gauge_ids, snapshot.Selection().lower_panel_ids})
    {
        const auto added = Add(LoggingMeasurementKind::kParameter, ids);
        if (!added.has_value())
        {
            return std::unexpected(added.error());
        }
    }
    const auto switches = Add(LoggingMeasurementKind::kSwitch, snapshot.Selection().switch_ids);
    if (!switches.has_value())
    {
        return std::unexpected(switches.error());
    }
    return columns;
}
} // namespace fastecu::logging
