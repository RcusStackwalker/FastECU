#include "src/ui/desktop/calibration/map_edit_adapter.h"

#include <bit>
#include <utility>

#include <QMdiSubWindow>
#include <QString>
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

const std::string& target_text(const calibration::MapCellValues& values, calibration::EditTargetKind kind)
{
    switch (kind)
    {
    case calibration::EditTargetKind::MapBody:
        return values.map_data;
    case calibration::EditTargetKind::XAxis:
        return values.x_axis_data;
    case calibration::EditTargetKind::YAxis:
        return values.y_axis_data;
    case calibration::EditTargetKind::Rejected:
        std::unreachable();
    }
    std::unreachable();
}

} // namespace

calibration::MapElementSpec MapElementFields::spec() const&
{
    calibration::MapElementSpec spec;
    spec.address = address_;
    spec.storage_type = storage_type_;
    spec.endian = endian_;
    spec.to_byte = to_byte_;
    spec.from_byte = from_byte_;
    spec.min_value = min_value_;
    spec.max_value = max_value_;
    spec.coarse_increment = coarse_increment_;
    spec.fine_increment = fine_increment_;
    spec.x_size = x_size_;
    spec.y_size = y_size_;
    spec.start_position = start_position_;
    spec.interval = interval_;
    spec.flash_method = flash_method_;
    spec.rom_file_size = rom_file_size_;
    return spec;
}

MapElementFields collect_map_element_fields(const calibration::CalibrationSession& session, int map_number,
                                            calibration::EditTargetKind kind)
{
    MapElementFields fields;
    const auto& def = session.definition()->definition;
    const auto& map = def.maps.at(static_cast<std::size_t>(map_number));
    const definition::Scaling *scaling = nullptr;
    if (kind == calibration::EditTargetKind::MapBody)
    {
        scaling = definition::find_scaling(def, map.scaling_name);
        fields.address_ = map.address.value_or(0);
        fields.storage_type_ = map.storage_type ? map.storage_type : scaling ? scaling->storage_type : std::nullopt;
        fields.endian_ = legacy_text(!map.endian.empty() ? map.endian : scaling ? scaling->endian : "");
        fields.from_byte_ = scaling ? legacy_text(scaling->from_byte) : " ";
        fields.to_byte_ = scaling ? legacy_text(scaling->to_byte) : " ";
        fields.start_position_ = map.start_position;
        fields.interval_ = map.interval;
    }
    else
    {
        if (kind == calibration::EditTargetKind::Rejected)
        {
            std::unreachable();
        }
        const auto& axis = kind == calibration::EditTargetKind::XAxis ? map.x_axis : map.y_axis;
        const bool present = !axis.type.empty();
        scaling = present ? definition::find_scaling(def, axis.scaling_name) : nullptr;
        fields.address_ = present ? axis.address.value_or(0) : 0;
        fields.storage_type_ = present ? axis.storage_type : std::nullopt;
        fields.endian_ = present ? legacy_text(axis.endian) : " ";
        fields.from_byte_ = present ? legacy_text(axis.from_byte) : " ";
        fields.to_byte_ = present ? legacy_text(axis.to_byte) : " ";
        fields.start_position_ = present ? axis.start_position : 1;
        fields.interval_ = present ? axis.interval : 1;
    }
    fields.min_value_ = scaling ? legacy_text(scaling->minimum) : " ";
    fields.max_value_ = scaling ? legacy_text(scaling->maximum) : " ";
    fields.coarse_increment_ = scaling ? QString::fromStdString(scaling->coarse_increment).toDouble() : 0.0;
    fields.fine_increment_ = scaling ? QString::fromStdString(scaling->fine_increment).toDouble() : 0.0;
    fields.x_size_ = map.x_size;
    fields.y_size_ = map.y_size;
    fields.flash_method_ = session.protocol().flash_method;
    fields.rom_file_size_ = session.protocol().unpadded_size;
    return fields;
}

QString format_raw_element_value(const calibration::MapElementSpec& spec, std::int64_t raw)
{
    // get_rom_data_value's storagetype.startsWith("float"/"uint"/"int")
    // chain matches nothing for a storage-type string outside the known set,
    // leaving `value` an empty (default-constructed) QString. storage_type_
    // from_text maps that same unrecognized string to std::nullopt, under
    // which is_unsigned_storage is false and storage_byte_size defaults to
    // 1 -- falling into the qint8 case below would return "-1"-like text
    // instead of legacy's "", a real divergence (observable in inc_dec_
    // value's `while (rom_data_value == new_rom_data_value)`), not one of
    // the established, deliberately-preserved defects. Handled first and
    // explicitly so it can't be missed among the branches below.
    if (!spec.storage_type.has_value())
    {
        return QString();
    }
    if (spec.storage_type == definition::StorageType::Float)
    {
        return QString::number(std::bit_cast<float>(static_cast<std::int32_t>(raw)));
    }
    if (definition::is_unsigned_storage(spec.storage_type))
    {
        return QString::number(static_cast<quint32>(raw));
    }
    switch (definition::storage_byte_size(spec.storage_type))
    {
    case 1:
        return QString::number(static_cast<qint8>(raw));
    case 2:
        return QString::number(static_cast<qint16>(raw));
    case 4:
        return QString::number(static_cast<qint32>(raw));
    default:
        return QString();
    }
}

std::int64_t raw_element_value_from_text(const calibration::MapElementSpec& spec, const QString& text)
{
    if (spec.storage_type == definition::StorageType::Float)
    {
        return static_cast<std::int64_t>(std::bit_cast<std::uint32_t>(text.toFloat()));
    }
    return static_cast<std::int64_t>(text.toInt());
}

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

ResolvedEdit::ResolvedEdit(MapElementFields fields, calibration::EditTarget target,
                           std::vector<std::string> owned_cell_text, int map_number)
    : fields_(std::move(fields)), target_(target), owned_cell_text_(std::move(owned_cell_text)), map_number_(map_number)
{
    cell_text_.reserve(owned_cell_text_.size());
    for (const auto& text : owned_cell_text_)
    {
        cell_text_.emplace_back(text);
    }
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
    if (target.kind == calibration::EditTargetKind::Rejected)
    {
        return std::nullopt;
    }
    const auto decoded = session.decode_map(static_cast<std::size_t>(map_number));
    if (!decoded.has_value())
    {
        return std::nullopt;
    }
    auto fields = collect_map_element_fields(session, map_number, target.kind);
    const QStringList parts = QString::fromStdString(target_text(*decoded, target.kind)).split(",");
    std::vector<std::string> owned_cell_text;
    owned_cell_text.reserve(static_cast<std::size_t>(parts.size()));
    for (const auto& part : parts)
    {
        owned_cell_text.push_back(part.toStdString());
    }

    return ResolvedEdit(std::move(fields), target, std::move(owned_cell_text), map_number);
}

Status apply_patch(calibration::CalibrationSession& session, int map_number, calibration::EditTargetKind kind,
                   const calibration::EditPatch& patch)
{
    if (kind == calibration::EditTargetKind::Rejected)
    {
        std::unreachable();
    }
    const auto decoded = session.decode_map(static_cast<std::size_t>(map_number));
    if (!decoded.has_value())
    {
        return std::unexpected(decoded.error());
    }
    // Preserve the legacy comma-split extent, including the trailing blank.
    const auto count = QString::fromStdString(target_text(*decoded, kind)).split(",").size();
    const auto size = session.rom().size();
    for (const auto& cell : patch)
    {
        if (cell.index >= static_cast<std::uint64_t>(count))
        {
            continue;
        }
        if (cell.byte_address > size || cell.bytes.size() > size - cell.byte_address)
        {
            return fail(ErrorKind::InvalidConfig, "map edit byte range is outside the ROM image");
        }
    }
    for (const auto& cell : patch)
    {
        // Skip a cell past the decoded extent before writing bytes, just as
        // the legacy text adapter did. resolve_edit_target no longer produces
        // the out-of-range (UINT32_MAX) index this once caught; the guard
        // stays as defense on the ROM write path.
        if (cell.index >= static_cast<std::uint64_t>(count))
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
