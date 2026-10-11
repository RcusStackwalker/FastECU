#include "src/ui/desktop/calibration/qt_calibration_interaction.h"

#include <QCoreApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QString>

namespace fastecu::ui
{

namespace
{

QString mainWindowText(const char *source)
{
    return QCoreApplication::translate("MainWindow", source);
}

} // namespace

QtCalibrationInteraction::QtCalibrationInteraction(QWidget *parent) : parent_(parent)
{
}

bool QtCalibrationInteraction::confirmWriteWithoutChecksum()
{
    QMessageBox msgBox(QMessageBox::Warning, "Checksum warning",
                       "WARNING! There is no checksum module for this ROM!\n"
                       "Be aware that if this ROM need checksum correction it must be done with another software!",
                       QMessageBox::Ok | QMessageBox::Cancel);
    return msgBox.exec() != QMessageBox::Cancel;
}

ChecksumCorrectionResult QtCalibrationInteraction::correctChecksums(const memory::MemoryImage& image,
                                                                    bool hasDefinition,
                                                                    const checksum::ChecksumSelection& selection)
{
    return checksum_command_.run(image, hasDefinition, selection, parent_);
}

std::optional<std::string> QtCalibrationInteraction::chooseSavePath(std::string_view suggestedPath)
{
    const QString path = QFileDialog::getSaveFileName(
        parent_, mainWindowText("Save calibration file"),
        QString::fromUtf8(suggestedPath.data(), static_cast<qsizetype>(suggestedPath.size())),
        mainWindowText("Calibration file (*.bin)"));
    if (path.isEmpty())
    {
        return std::nullopt;
    }
    return path.toStdString();
}

void QtCalibrationInteraction::showNotice(CalibrationNotice notice)
{
    switch (notice)
    {
    case CalibrationNotice::kNoCalibrationToWrite:
        QMessageBox::warning(parent_, mainWindowText("Write ROM"), "No file selected!");
        break;
    case CalibrationNotice::kNoCalibrationToSave:
        QMessageBox::information(parent_, mainWindowText("Calibration file"), "No calibration to save!");
        break;
    case CalibrationNotice::kNoSaveFilename:
        QMessageBox::information(parent_, mainWindowText("Calibration file"), "No file name selected");
        break;
    }
}

} // namespace fastecu::ui
