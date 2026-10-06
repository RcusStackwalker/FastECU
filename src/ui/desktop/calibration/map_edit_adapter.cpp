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
    case calibration::EditTargetKind::MapBody:
        return calibration::NumericTarget::MapBody;
    case calibration::EditTargetKind::XAxis:
        return calibration::NumericTarget::XAxis;
    case calibration::EditTargetKind::YAxis:
        return calibration::NumericTarget::YAxis;
    case calibration::EditTargetKind::Rejected:
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
    QTableWidget *table = window->findChild<QTableWidget *>(window->objectName());
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
