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

Result<MapPresentation> presentMap(const calibration::CalibrationSession& session, std::size_t index);
QString formatMapValue(double value, const QString& format);
int selectionIndex(const PresentedCell& cell);
std::optional<MapColorBounds> openingColorBounds(const MapPresentation& map);
QColor mapCellColor(double value, MapColorBounds bounds);

} // namespace fastecu::ui
