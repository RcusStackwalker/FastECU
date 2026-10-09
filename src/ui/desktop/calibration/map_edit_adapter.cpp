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

std::optional<calibration::NumericTarget> to_numeric_target(calibration::EditTargetKind kind)
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

std::optional<MapWindowId> parse_map_window_id(QMdiSubWindow *window)
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
    const std::optional<calibration::SessionId> session = parse_session_key(parts.at(0));
    if (!session.has_value())
    {
        return std::nullopt;
    }
    return MapWindowId{.session = *session, .map_number = parts.at(1).toInt()};
}

std::optional<calibration::NumericSelection>
selected_numeric_target(QMdiSubWindow *window, const calibration::CalibrationSession& session, int map_number)
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
    if (!session.definition() || map_number < 0 ||
        static_cast<std::size_t>(map_number) >= session.definition()->definition.maps.size())
    {
        return std::nullopt;
    }
    const auto& first = selected.first();
    const auto& map = session.definition()->definition.maps[static_cast<std::size_t>(map_number)];
    const auto target = calibration::resolve_edit_target({.first_row = first.topRow(),
                                                          .first_col = first.leftColumn(),
                                                          .last_row = first.bottomRow(),
                                                          .last_col = first.rightColumn()},
                                                         {.x_size = map.x_size, .y_size = map.y_size}, map.x_axis.type);
    const auto numeric_target = to_numeric_target(target.kind);
    if (!numeric_target.has_value())
    {
        return std::nullopt;
    }
    return calibration::NumericSelection{.target = *numeric_target, .elements = target.range};
}

std::optional<calibration::SelectionRange> body_widget_range(const calibration::CalibrationSession& session,
                                                             int map_number, int rows, int columns)
{
    if (!session.definition() || map_number < 0 ||
        static_cast<std::size_t>(map_number) >= session.definition()->definition.maps.size())
    {
        return std::nullopt;
    }
    const auto& map = session.definition()->definition.maps[static_cast<std::size_t>(map_number)];
    const auto is_body = [&](int row, int col)
    {
        const auto target =
            calibration::resolve_edit_target({.first_row = row, .first_col = col, .last_row = row, .last_col = col},
                                             {.x_size = map.x_size, .y_size = map.y_size}, map.x_axis.type);
        return target.kind == calibration::EditTargetKind::kMapBody;
    };
    for (int row = 0; row < rows; ++row)
    {
        for (int col = 0; col < columns; ++col)
        {
            if (is_body(row, col))
            {
                return calibration::SelectionRange{
                    .first_row = row, .first_col = col, .last_row = rows - 1, .last_col = columns - 1};
            }
        }
    }
    return std::nullopt;
}

std::vector<std::vector<std::string>> split_paste_rows(const QString& text)
{
    QStringList rows = text.split('\n');
    // A terminal record delimiter does not create another numeric row.
    // Keep interior empty rows so malformed input still rejects atomically.
    if (text.endsWith('\n'))
    {
        rows.removeLast();
    }
    std::vector<std::vector<std::string>> owned_rows;
    owned_rows.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto& row : rows)
    {
        const QStringList columns = row.split('\t');
        std::vector<std::string> owned_columns;
        owned_columns.reserve(static_cast<std::size_t>(columns.size()));
        for (const auto& column : columns)
        {
            owned_columns.push_back(column.toStdString());
        }
        owned_rows.push_back(std::move(owned_columns));
    }
    return owned_rows;
}

} // namespace fastecu::ui
