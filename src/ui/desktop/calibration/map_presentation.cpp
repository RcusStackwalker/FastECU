#include "src/ui/desktop/calibration/map_presentation.h"

#include <algorithm>

namespace fastecu::ui
{
namespace
{
QString text(const std::string& value)
{
    return value.empty() ? QString(" ") : QString::fromStdString(value);
}
} // namespace

Result<MapPresentation> present_map(const calibration::CalibrationSession& session, std::size_t index)
{
    const auto decoded = session.decode_map(index);
    if (!decoded.has_value())
    {
        return std::unexpected(decoded.error());
    }
    const auto& definition = session.definition()->definition;
    const auto& map = definition.maps[index];
    const auto *scaling = definition::find_scaling(definition, map.scaling_name);
    const auto *x_scaling = definition::find_scaling(definition, map.x_axis.scaling_name);
    const auto *y_scaling = definition::find_scaling(definition, map.y_axis.scaling_name);
    const bool x_present = !map.x_axis.type.empty();
    const bool y_present = !map.y_axis.type.empty();
    MapPresentation result{
        .name = text(map.name),
        .type = text(map.type),
        .units = scaling != nullptr ? text(scaling->units) : QString(" "),
        .format = scaling != nullptr ? text(scaling->format) : QString(" "),
        .x_type = text(map.x_axis.type),
        .x_name = x_present ? text(map.x_axis.name) : QString(" "),
        .x_units = x_present ? text(map.x_axis.units) : QString(" "),
        .x_format = x_present && x_scaling != nullptr ? text(x_scaling->format) : QString(" "),
        .y_name = y_present ? text(map.y_axis.name) : QString(" "),
        .y_units = y_present ? text(map.y_axis.units) : QString(" "),
        .y_format = y_present && y_scaling != nullptr ? text(y_scaling->format) : QString(" "),
        .body = QString::fromStdString(decoded->map_data).split(','),
        .x_axis = QString::fromStdString(decoded->x_axis_data).split(','),
        .y_axis = QString::fromStdString(decoded->y_axis_data).split(','),
        .x_size = static_cast<int>(map.x_size),
        .y_size = static_cast<int>(map.y_size),
    };
    if (scaling != nullptr && !scaling->selections.empty())
    {
        for (const auto& [name, value] : scaling->selections)
        {
            result.selection_names.append(QString::fromStdString(name));
            result.selection_values.append(QString::fromStdString(value));
        }
        result.selection_names.append("");
        result.selection_values.append("");
    }
    else
    {
        result.selection_names.append(" ");
        result.selection_values.append(" ");
    }
    return result;
}

QString format_map_value(const QString& value, const QString& format)
{
    const auto decimals = static_cast<int>(format.contains('.') ? format.split('.').at(1).count(QLatin1Char('0')) : 0);
    return QString::number(value.toFloat(), 'f', decimals);
}

MapColorBounds opening_color_bounds(const MapPresentation& map)
{
    // Preserve the opening display's float conversion and QString::number
    // rounding, including for constant ranges. The window keeps these bounds.
    const auto bound = [](const QString& value) { return QString::number(value.toFloat()).toFloat(); };
    MapColorBounds result{bound(map.body.value(0)), bound(map.body.value(0))};
    for (int i = 0; i < map.x_size * map.y_size; ++i)
    {
        const float value = map.body.value(i).toFloat();
        if (value < result.minimum)
        {
            result.minimum = bound(map.body.value(i));
        }
        if (value > result.maximum)
        {
            result.maximum = bound(map.body.value(i));
        }
    }
    return result;
}

QColor map_cell_color(float value, MapColorBounds bounds)
{
    constexpr double kScale = 210.0 / 360.0;
    const double hue =
        bounds.maximum == bounds.minimum
            ? 0.0
            : std::clamp(kScale * (value - bounds.minimum) / (bounds.maximum - bounds.minimum), 0.0, kScale);
    return QColor::fromHsvF(static_cast<float>(hue), 0.85F, 0.85F);
}

} // namespace fastecu::ui
