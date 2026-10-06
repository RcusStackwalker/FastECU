#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <utility>
#include <algorithm>
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

std::string legacy_text(std::string_view text)
{
    return text.empty() ? " " : std::string(text);
}

const calibration::NumericRun *target_cells(const calibration::DecodedMap& values, calibration::EditTargetKind kind)
{
    switch (kind)
    {
    case calibration::EditTargetKind::MapBody:
        return std::get_if<calibration::NumericRun>(&values.body);
    case calibration::EditTargetKind::XAxis:
        return std::get_if<calibration::NumericRun>(&values.x_axis);
    case calibration::EditTargetKind::YAxis:
        return std::get_if<calibration::NumericRun>(&values.y_axis);
    case calibration::EditTargetKind::Rejected:
        return nullptr;
    }
    return nullptr;
}

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
    const auto target =
        calibration::resolve_edit_target({.first_row = first.topRow(),
                                          .first_col = first.leftColumn(),
                                          .last_row = first.bottomRow(),
                                          .last_col = first.rightColumn()},
                                         {.x_size = map.x_size, .y_size = map.y_size}, legacy_text(map.x_axis.type));
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

ResolvedEdit::ResolvedEdit(calibration::MapElementFields fields, calibration::EditTarget target,
                           calibration::NumericRun cells, int map_number)
    : fields_(std::move(fields)), target_(target), cells_(std::move(cells)), map_number_(map_number)
{
}

std::optional<ResolvedEdit> resolve_active_map_edit(QMdiSubWindow *window,
                                                    const calibration::CalibrationSession& session, int map_number)
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
    const auto& first = selected.first();

    const calibration::SelectionRange selection{.first_row = first.topRow(),
                                                .first_col = first.leftColumn(),
                                                .last_row = first.bottomRow(),
                                                .last_col = first.rightColumn()};
    if (!session.definition() || map_number < 0 ||
        static_cast<std::size_t>(map_number) >= session.definition()->definition.maps.size())
    {
        return std::nullopt;
    }
    const auto& map = session.definition()->definition.maps[static_cast<std::size_t>(map_number)];
    const calibration::MapDimensions dims{.x_size = map.x_size, .y_size = map.y_size};
    const auto target = calibration::resolve_edit_target(selection, dims, legacy_text(map.x_axis.type));
    const auto numeric_target = to_numeric_target(target.kind);
    if (!numeric_target.has_value())
    {
        return std::nullopt;
    }
    const auto decoded = session.decode_map(static_cast<std::size_t>(map_number));
    if (!decoded.has_value())
    {
        return std::nullopt;
    }
    const auto *cells = target_cells(*decoded, target.kind);
    if (cells == nullptr)
    {
        return std::nullopt;
    }
    auto fields =
        calibration::collect_map_element_fields(session, static_cast<std::size_t>(map_number), *numeric_target);
    return ResolvedEdit(std::move(fields), target, *cells, map_number);
}

Status apply_patch(calibration::CalibrationSession& session, int map_number, calibration::EditTargetKind kind,
                   const calibration::NumericEditPatch& patch)
{
    const auto decoded = session.decode_map(static_cast<std::size_t>(map_number));
    if (!decoded.has_value())
    {
        return std::unexpected(decoded.error());
    }
    const auto *cells = target_cells(*decoded, kind);
    if (cells == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "edit target is not a numeric run");
    }
    const auto numeric_target = to_numeric_target(kind);
    if (!numeric_target.has_value())
    {
        return fail(ErrorKind::InvalidConfig, "edit target is not a numeric run");
    }
    const auto fields =
        calibration::collect_map_element_fields(session, static_cast<std::size_t>(map_number), *numeric_target);
    const auto spec = fields.spec();
    const auto width = definition::storage_byte_size(spec.storage_type);
    const auto size = session.rom().size();
    for (const auto& cell : patch)
    {
        if (cell.index >= cells->cells.size() || cell.bytes.size() != width ||
            cell.byte_address != calibration::element_byte_address(spec, cell.index, true))
        {
            return fail(ErrorKind::InvalidConfig, "map edit index, address, or byte width does not match its target");
        }
        if (cell.byte_address > size || cell.bytes.size() > size - cell.byte_address)
        {
            return fail(ErrorKind::InvalidConfig, "map edit byte range is outside the ROM image");
        }
    }
    for (const auto& cell : patch)
    {
        const auto current = session.rom().subspan(static_cast<std::size_t>(cell.byte_address), cell.bytes.size());
        if (std::ranges::equal(current, cell.bytes))
        {
            continue;
        }
        const auto written = session.write_bytes(cell.byte_address, cell.bytes);
        if (!written.has_value())
        {
            return written;
        }
    }
    return {};
}

} // namespace fastecu::ui
