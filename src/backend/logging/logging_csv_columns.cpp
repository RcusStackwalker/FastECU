#include "src/backend/logging/logging_csv_columns.h"

namespace fastecu::logging
{
Result<std::vector<LoggingCsvColumn>> prepare_logging_csv_columns(const LoggingRunSnapshot& snapshot)
{
    std::vector<LoggingCsvColumn> columns;
    const auto add = [&](LoggingMeasurementKind kind, const auto& ids) -> Status
    {
        for (const auto& id : ids)
        {
            const auto *measurement = snapshot.find_measurement(kind, id);
            if (measurement == nullptr)
            {
                return fail(ErrorKind::Internal, "CSV selection has no captured measurement");
            }
            columns.push_back({kind, measurement->identity, measurement->name});
        }
        return {};
    };
    for (const auto& ids : {snapshot.selection().gauge_ids, snapshot.selection().lower_panel_ids})
    {
        const auto added = add(LoggingMeasurementKind::Parameter, ids);
        if (!added.has_value())
        {
            return std::unexpected(added.error());
        }
    }
    const auto switches = add(LoggingMeasurementKind::Switch, snapshot.selection().switch_ids);
    if (!switches.has_value())
    {
        return std::unexpected(switches.error());
    }
    return columns;
}
} // namespace fastecu::logging
