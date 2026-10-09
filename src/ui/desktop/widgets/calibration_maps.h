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

    int map_cell_width_selectable = 240;
    int map_cell_width1_d = 96;
    int map_cell_width = 54;
    int map_cell_height = 26;
    int cell_font_size = static_cast<int>(map_cell_height / 2.35);

    int start_col = 0;
    int start_row = 0;
    int x_size = 0;
    int y_size = 0;
    int x_size_offset = 0;
    int y_size_offset = 0;

  protected:
    // Select All on a numeric table selects the body, not the axes.
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void initializeView(const fastecu::ui::MapPresentation& map, const fastecu::calibration::RomSource& source);
    void showMapError(const fastecu::Error& error);
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
    void selectableComboboxItemChanged(QString);
    void checkboxStateChanged(int);

  private:
    fastecu::calibration::CalibrationWorkspace& workspace_;
    fastecu::calibration::SessionId session_;
    int map_index_;
    std::optional<fastecu::ui::MapColorBounds> color_bounds_;
    QRect mdi_area_size_;
    bool view_initialized_{false};
    bool numeric_body_{false};
    QLabel *map_error_label_{nullptr};
    std::unique_ptr<Ui::CalibrationMaps> ui_;
};
