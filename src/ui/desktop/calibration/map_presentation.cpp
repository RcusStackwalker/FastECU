#include "src/ui/desktop/calibration/map_presentation.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "src/backend/definition/text_format.h"

namespace fastecu::ui
{
namespace
{
QString text(const std::string& value)
{
    return value.empty() ? QString(" ") : QString::fromStdString(value);
}

std::vector<PresentedCell> present_numeric(const calibration::NumericRun& run, const QString& format)
{
    std::vector<PresentedCell> cells;
    cells.reserve(run.cells.size());
    for (const auto& cell : run.cells)
    {
        if (cell.has_value())
        {
            cells.push_back({.text = format_map_value(*cell, format), .numeric_value = *cell});
        }
        else
        {
            cells.push_back({.text = "NaN", .diagnostic = QString::fromStdString(cell.error().detail)});
        }
    }
    return cells;
}

std::optional<std::vector<PresentedCell>> present_axis(const calibration::AxisValue& axis, const QString& format)
{
    if (const auto *numeric = std::get_if<calibration::NumericRun>(&axis))
    {
        return present_numeric(*numeric, format);
    }
    if (const auto *labels = std::get_if<calibration::StaticAxis>(&axis))
    {
        std::vector<PresentedCell> cells;
        for (const auto& label : labels->labels)
        {
            cells.push_back({.text = QString::fromStdString(label)});
        }
        return cells;
    }
    return std::nullopt;
}

Result<bytes::Bytes> selection_bytes(std::string_view value)
{
    if (value.size() % 2 != 0)
    {
        return fail(ErrorKind::InvalidConfig, "selection hex value has an incomplete byte");
    }
    bytes::Bytes data;
    data.reserve(value.size() / 2);
    for (std::size_t offset = 0; offset < value.size(); offset += 2)
    {
        const auto byte = definition::parse_hex_value(value.substr(offset, 2));
        if (!byte.has_value() || *byte > 255)
        {
            return fail(ErrorKind::InvalidConfig, "selection value is not hexadecimal bytes");
        }
        data.push_back(static_cast<bytes::Byte>(*byte));
    }
    return data;
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
        .x_size = static_cast<int>(map.x_size),
        .y_size = static_cast<int>(map.y_size),
    };
    if (const auto *numeric = std::get_if<calibration::NumericRun>(&decoded->body))
    {
        result.body = present_numeric(*numeric, result.format);
    }
    else
    {
        result.blob = std::get<calibration::BlobValue>(decoded->body).data;
        result.body.push_back({.text = QString::fromStdString(bytes::toHex(*result.blob, "{:02x}"))});
    }
    result.x_axis = present_axis(decoded->x_axis, result.x_format);
    result.y_axis = present_axis(decoded->y_axis, result.y_format);
    if (scaling != nullptr)
    {
        for (const auto& [name, value] : scaling->selections)
        {
            auto data = selection_bytes(value);
            if (!data.has_value())
            {
                return fail(data.error().kind,
                            std::format("map '{}' selection '{}': {}", map.name, name, data.error().detail));
            }
            result.selection_names.append(QString::fromStdString(name));
            result.selection_values.push_back(std::move(*data));
        }
    }
    return result;
}

int selection_index(const PresentedCell& cell)
{
    if (!cell.numeric_value.has_value())
    {
        return 0;
    }
    const double value = *cell.numeric_value;
    if (!std::isfinite(value) || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max() ||
        std::trunc(value) != value)
    {
        return 0;
    }
    return static_cast<int>(value);
}

QString format_map_value(double value, const QString& format)
{
    const auto decimals = static_cast<int>(format.contains('.') ? format.split('.').at(1).count(QLatin1Char('0')) : 0);
    return QString::number(value, 'f', decimals);
}

std::optional<MapColorBounds> opening_color_bounds(const MapPresentation& map)
{
    std::optional<MapColorBounds> bounds;
    for (const auto& cell : map.body)
    {
        if (!cell.numeric_value.has_value())
        {
            continue;
        }
        if (!bounds.has_value())
        {
            bounds = MapColorBounds{*cell.numeric_value, *cell.numeric_value};
        }
        else
        {
            bounds->minimum = std::min(bounds->minimum, *cell.numeric_value);
            bounds->maximum = std::max(bounds->maximum, *cell.numeric_value);
        }
    }
    return bounds;
}

QColor map_cell_color(double value, MapColorBounds bounds)
{
    constexpr double kScale = 210.0 / 360.0;
    double fraction = 0.0;
    if (bounds.maximum != bounds.minimum)
    {
        const double bounded = std::clamp(value, bounds.minimum, bounds.maximum);
        const double range = bounds.maximum - bounds.minimum;
        if (std::isfinite(range))
        {
            fraction = (bounded - bounds.minimum) / range;
        }
        else
        {
            const double scale = std::max(std::abs(bounds.minimum), std::abs(bounds.maximum));
            fraction = (bounded / scale - bounds.minimum / scale) / (bounds.maximum / scale - bounds.minimum / scale);
        }
    }
    const double hue = kScale * std::clamp(fraction, 0.0, 1.0);
    return QColor::fromHsvF(static_cast<float>(hue), 0.85F, 0.85F);
}
} // namespace fastecu::ui
