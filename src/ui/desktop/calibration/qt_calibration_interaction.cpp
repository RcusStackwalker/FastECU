#include "src/ui/desktop/calibration/qt_calibration_interaction.h"

#include <QCoreApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QString>

namespace fastecu::ui
{

namespace
{

QString main_window_text(const char *source)
{
    return QCoreApplication::translate("MainWindow", source);
}

} // namespace

QtCalibrationInteraction::QtCalibrationInteraction(QWidget *parent) : parent_(parent)
{
}

bool QtCalibrationInteraction::confirm_write_without_checksum()
{
    QMessageBox msgBox(QMessageBox::Warning, "Checksum warning",
                       "WARNING! There is no checksum module for this ROM!\n"
                       "Be aware that if this ROM need checksum correction it must be done with another software!",
                       QMessageBox::Ok | QMessageBox::Cancel);
    return msgBox.exec() != QMessageBox::Cancel;
}

ChecksumCorrectionResult QtCalibrationInteraction::correct_checksums(bytes::ByteView image, bool has_definition,
                                                                     const checksum::ChecksumSelection& selection)
{
    return checksum_command_.run(image, has_definition, selection, parent_);
}

std::optional<std::string> QtCalibrationInteraction::choose_save_path(std::string_view suggested_path)
{
    const QString path = QFileDialog::getSaveFileName(
        parent_, main_window_text("Save calibration file"),
        QString::fromUtf8(suggested_path.data(), static_cast<qsizetype>(suggested_path.size())),
        main_window_text("Calibration file (*.bin)"));
    if (path.isEmpty())
    {
        return std::nullopt;
    }
    return path.toStdString();
}

void QtCalibrationInteraction::show_notice(CalibrationNotice notice)
{
    switch (notice)
    {
    case CalibrationNotice::kNoCalibrationToWrite:
        QMessageBox::warning(parent_, main_window_text("Write ROM"), "No file selected!");
        break;
    case CalibrationNotice::kNoCalibrationToSave:
        QMessageBox::information(parent_, main_window_text("Calibration file"), "No calibration to save!");
        break;
    case CalibrationNotice::kNoSaveFilename:
        QMessageBox::information(parent_, main_window_text("Calibration file"), "No file name selected");
        break;
    }
}

} // namespace fastecu::ui
