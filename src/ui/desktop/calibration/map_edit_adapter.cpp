#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <utility>
#include <cstddef>
#include <optional>

#include <QMdiSubWindow>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetSelectionRange>

namespace fastecu::ui
{
namespace
{

std::optional<calibration::NumericTarget> toNumericTarget(calibration::EditTargetKind kind)
{
    switch (kind)
    {
    case calibration::EditTargetKind::kMapBody:
        return calibration::NumericTarget::kMapBody;
    case calibration::EditTargetKind::kXAxis:
        return calibration::NumericTarget::kXAxis;
    case calibration::EditTargetKind::kYAxis:
        return calibration::NumericTarget::kYAxis;
    case calibration::EditTargetKind::kRejected:
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

std::optional<MapWindowId> parseMapWindowId(QMdiSubWindow *window)
{
    if (!window)
    {
        return std::nullopt;
    }
    const QStringList parts = window->objectName().split(",");
    if (parts.size() < 2)
    {
        return std::nullopt;
    }
    const std::optional<calibration::SessionId> session = parseSessionKey(parts.at(0));
    if (!session.has_value())
    {
        return std::nullopt;
    }
    return MapWindowId{.session = *session, .map_number = parts.at(1).toInt()};
}

std::optional<calibration::NumericSelection>
selectedNumericTarget(QMdiSubWindow *window, const calibration::CalibrationSession& session, int mapNumber)
{
    if (!window)
    {
        return std::nullopt;
    }
    QTableWidget *table = window->findChild<QTableWidget *>();
    if (!table)
    {
        return std::nullopt;
    }
    const auto selected = table->selectedRanges();
    if (selected.isEmpty())
    {
        return std::nullopt;
    }
    if (!session.Definition() || mapNumber < 0 ||
        static_cast<std::size_t>(mapNumber) >= session.Definition()->definition.maps.size())
    {
        return std::nullopt;
    }
    const auto& first = selected.first();
    const auto& map = session.Definition()->definition.maps[static_cast<std::size_t>(mapNumber)];
    const auto target = calibration::ResolveEditTarget({.first_row = first.topRow(),
                                                        .first_col = first.leftColumn(),
                                                        .last_row = first.bottomRow(),
                                                        .last_col = first.rightColumn()},
                                                       {.x_size = map.x_size, .y_size = map.y_size}, map.x_axis.type);
    const auto numericTarget = toNumericTarget(target.kind);
    if (!numericTarget.has_value())
    {
        return std::nullopt;
    }
    return calibration::NumericSelection{.target = *numericTarget, .elements = target.range};
}

std::optional<calibration::SelectionRange> bodyWidgetRange(const calibration::CalibrationSession& session,
                                                           int mapNumber, int rows, int columns)
{
    if (!session.Definition() || mapNumber < 0 ||
        static_cast<std::size_t>(mapNumber) >= session.Definition()->definition.maps.size())
    {
        return std::nullopt;
    }
    const auto& map = session.Definition()->definition.maps[static_cast<std::size_t>(mapNumber)];
    const auto isBody = [&](int row, int col)
    {
        const auto target =
            calibration::ResolveEditTarget({.first_row = row, .first_col = col, .last_row = row, .last_col = col},
                                           {.x_size = map.x_size, .y_size = map.y_size}, map.x_axis.type);
        return target.kind == calibration::EditTargetKind::kMapBody;
    };
    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < columns; ++col)
        {
            if (isBody(row, col))
            {
                return calibration::SelectionRange{
                    .first_row = row, .first_col = col, .last_row = rows - 1, .last_col = columns - 1};
            }
        }
    }
    return std::nullopt;
}

std::vector<std::vector<std::string>> splitPasteRows(const QString& text)
{
    QStringList rows = text.split('\n');
    // A terminal record delimiter does not create another numeric row.
    // Keep interior empty rows so malformed input still rejects atomically.
    if (text.endsWith('\n'))
    {
        rows.removeLast();
    }
    std::vector<std::vector<std::string>> ownedRows;
    ownedRows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows)
    {
        const QStringList columns = row.split('\t');
        std::vector<std::string> ownedColumns;
        ownedColumns.reserve(static_cast<std::size_t>(columns.size()));
        for (const auto& column : columns)
        {
            ownedColumns.push_back(column.toStdString());
        }
        ownedRows.push_back(std::move(ownedColumns));
    }
    return ownedRows;
}

} // namespace fastecu::ui
