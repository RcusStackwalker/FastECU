#pragma once

#include <QFile>
#include <QFileInfo>
#include <QTreeWidget>
#include <QLabel>
#include <QPushButton>

#include "src/backend/calibration/session/calibration_session.h"
#include "src/ui/desktop/calibration/calibration_view_state.h"
#include "src/ui/desktop/calibration/session_key.h"

class CalibrationTreeWidget : public QWidget
{
    Q_OBJECT

  signals:
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

  public:
    CalibrationTreeWidget();

    QTreeWidget *buildCalibrationFilesTree(fastecu::calibration::SessionId sessionId, QTreeWidget *filesTreeWidget,
                                           const fastecu::calibration::CalibrationSession& session);
    QTreeWidget *buildCalibrationDataTree(QTreeWidget *dataTreeWidget,
                                          const fastecu::calibration::CalibrationSession& session,
                                          const fastecu::ui::CalibrationViewState& view);
    /*
        QStringList RomInfoStrings = {
            "XmlId",
            "InternalIdAddress",
            "Make",
            "Model",
            "Submodel",
            "Market",
            "Transmission",
            "Year",
            "ECU ID",
            "Internal ID",
            "Memory Model",
            "Checksum Module",
            "Rom Base",
            "Flash Method",
            "File Size",
            "Def File",
        };

        enum RomInfoEnum {
            XmlId,
            InternalIdAddress,
            Make,
            Model,
            SubModel,
            Market,
            Transmission,
            Year,
            EcuId,
            InternalIdString,
            MemModel,
            ChecksumModule,
            RomBase,
            FlashMethod,
            FileSize,
            DefFile,
        };
    */
  signals:
    void closeRom();

  private:
  private slots:
};
