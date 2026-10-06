#pragma once

#include <memory>
#include <optional>

#include <QMainWindow>
#include <QDebug>
#include <QWidget>
#include <QString>
#include <QMdiSubWindow>
#include <QComboBox>
#include <QCheckBox>

#include "src/ui/desktop/calibration/session_key.h"
#include "src/backend/calibration/session/calibration_workspace.h"
#include "src/ui/desktop/calibration/map_presentation.h"
#include "src/ui/desktop/widgets/verticallabel.h"

class QLabel;

QT_BEGIN_NAMESPACE
namespace Ui
{
class CalibrationMaps;
}
QT_END_NAMESPACE

class CalibrationMaps : public QWidget
{
    Q_OBJECT

  public:
    explicit CalibrationMaps(fastecu::calibration::CalibrationWorkspace& workspace,
                             fastecu::calibration::SessionId session, int mapIndex, QRect mdiAreaSize,
                             QWidget *parent = nullptr);
    ~CalibrationMaps();

    int mapCellWidthSelectable = 240;
    int mapCellWidth1D = 96;
    int mapCellWidth = 54;
    int mapCellHeight = 26;
    int cellFontSize = static_cast<int>(mapCellHeight / 2.35);

    int startCol = 0;
    int startRow = 0;
    int xSize = 0;
    int ySize = 0;
    int xSizeOffset = 0;
    int ySizeOffset = 0;

  private:
    void initialize_view(const fastecu::ui::MapPresentation& map, const fastecu::calibration::RomSource& source);
    void show_map_error(const fastecu::Error& error);
    void setMapTableWidgetSize(int maxWidth, int maxHeight, int sizeX);

  public:
    void refresh(); // Resolves the stable identity; a closed session is inert.

  private slots:
    // void fetchFromEcu();
    // void storeToEcu();
    void cellClicked(int row, int col);
    void cellPressed(int row, int col);
    void cellChanged(int curRow, int curCol, int prevRow, int prevCol);

  signals:
    void fetchFromEcuButtonClicked();
    void storeToEcuButtonClicked();
    void selectable_combobox_item_changed(QString);
    void checkbox_state_changed(int);

  private:
    fastecu::calibration::CalibrationWorkspace& workspace_;
    fastecu::calibration::SessionId session_;
    int map_index_;
    std::optional<fastecu::ui::MapColorBounds> color_bounds_;
    QRect mdi_area_size_;
    bool view_initialized_{false};
    QLabel *map_error_label_{nullptr};
    std::unique_ptr<Ui::CalibrationMaps> ui;
};
