#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

#include "src/backend/calibration/session/calibration_session.h"

namespace fastecu::ui
{

// A transient presentation of one decoded map. Never stores ROM bytes or
// editable values: build it again from the session on each refresh.
struct MapPresentation
{
    QString name;
    QString type;
    QString units;
    QString format;
    QString x_type;
    QString x_name;
    QString x_units;
    QString x_format;
    QString y_name;
    QString y_units;
    QString y_format;
    QStringList body;
    QStringList x_axis;
    QStringList y_axis;
    QStringList selection_names;
    QStringList selection_values;
    int x_size{1};
    int y_size{1};
};

struct MapColorBounds
{
    float minimum{0};
    float maximum{0};
};

Result<MapPresentation> present_map(const calibration::CalibrationSession& session, std::size_t index);
QString format_map_value(const QString& value, const QString& format);
MapColorBounds opening_color_bounds(const MapPresentation& map);
QColor map_cell_color(float value, MapColorBounds bounds);

} // namespace fastecu::ui
