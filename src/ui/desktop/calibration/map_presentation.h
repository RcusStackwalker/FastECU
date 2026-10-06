#pragma once

#include <QColor>
#include <QString>
#include <QStringList>

#include "src/backend/calibration/session/calibration_session.h"

namespace fastecu::ui
{
struct PresentedCell
{
    QString text;
    QString diagnostic;
    std::optional<double> numeric_value;
};

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
    std::vector<PresentedCell> body;
    std::optional<std::vector<PresentedCell>> x_axis;
    std::optional<std::vector<PresentedCell>> y_axis;
    std::optional<bytes::Bytes> blob;
    QStringList selection_names;
    std::vector<bytes::Bytes> selection_values;
    int x_size{1};
    int y_size{1};
};

struct MapColorBounds
{
    double minimum{0};
    double maximum{0};
};

Result<MapPresentation> present_map(const calibration::CalibrationSession& session, std::size_t index);
QString format_map_value(double value, const QString& format);
int selection_index(const PresentedCell& cell);
std::optional<MapColorBounds> opening_color_bounds(const MapPresentation& map);
QColor map_cell_color(double value, MapColorBounds bounds);

} // namespace fastecu::ui
