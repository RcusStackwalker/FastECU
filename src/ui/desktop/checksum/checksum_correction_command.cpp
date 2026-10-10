#include "src/ui/desktop/checksum/checksum_correction_command.h"

#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QWidget>

#include "src/backend/checksum/dispatch.h"
#include "src/backend/flash/flash_device_lookup.h"

namespace fastecu::ui
{

using fastecu::checksum::ChecksumCorrectionOutcome;
// ChecksumResult needs no using-declaration: it is in the global namespace.

bool ChecksumCorrectionCommand::confirmProceedWithoutDefinition(QWidget *parent)
{
    QMessageBox msgBox(parent);
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle("Calibration file");
    msgBox.setText(
        "WARNING! No definition file linked to selected ROM, checksums are not calculated!\n\n"
        "If you are sure that right protocol is selected and want to correct checksums anyway, press 'DO IT!' -button");
    QPushButton *okButton = msgBox.addButton(QMessageBox::Ok);
    msgBox.addButton(QObject::tr("DO IT!"), QMessageBox::NoRole);
    msgBox.exec();
    return msgBox.clickedButton() != okButton;
}

void ChecksumCorrectionCommand::showBadRomSizeDialog(QWidget *parent)
{
    QMessageBox::information(parent, QObject::tr("Checksum module"),
                             "Bad ROM size! Make sure that you have selected correct flash method!");
}

bool ChecksumCorrectionCommand::confirmProceedWithoutChecksumModule()
{
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setWindowTitle("File - Checksum Warning");
    msgBox.setText("WARNING! There is no checksum module for this ROM!"
                   "                            Be aware that if this ROM need checksum correction it must be done "
                   "with another software!");
    QPushButton *cancelButton = msgBox.addButton(QMessageBox::Cancel);
    msgBox.addButton(QMessageBox::Ok);
    msgBox.exec();
    return msgBox.clickedButton() == cancelButton;
}

void ChecksumCorrectionCommand::showFamilyResultDialog(const ChecksumResult& familyResult)
{
    const QString message = QString::fromStdString(familyResult.message);
    if (familyResult.Changed())
    {
        QMessageBox::information(nullptr, QObject::tr("Checksum Correction"),
                                 QObject::tr("Checksums corrected:\n\n%1").arg(message));
        return;
    }
    switch (familyResult.status)
    {
    case ChecksumResult::Status::kDisabled:
        QMessageBox::information(nullptr, QObject::tr("32-bit checksum"), message);
        break;
    case ChecksumResult::Status::kInvalidSize:
    case ChecksumResult::Status::kUnsupportedRom:
    case ChecksumResult::Status::kParseError:
        QMessageBox::warning(nullptr, QObject::tr("Checksum module"), message);
        break;
    case ChecksumResult::Status::kCorrected:
    case ChecksumResult::Status::kUnchanged:
        break;
    }
}

ChecksumCorrectionResult ChecksumCorrectionCommand::run(const fastecu::memory::MemoryImage& image, bool hasDefinition,
                                                        const fastecu::checksum::ChecksumSelection& selection,
                                                        QWidget *parent)
{
    ChecksumCorrectionResult result;

    // Formerly FileActions::checksum_correction's own precheck, ahead of the
    // adapter call: an unregistered MCU returns the ROM untouched and shows
    // no dialog at all.
    if (fastecu::flash::FindFlashDevice(selection.mcu_type) == nullptr)
    {
        result.unknown_mcu_type = true;
        return result;
    }

    if (!hasDefinition)
    {
        if (!confirmProceedWithoutDefinition(parent))
        {
            return result;
        }
    }

    switch (const ChecksumCorrectionOutcome outcome = fastecu::checksum::ApplyChecksumCorrection(image, selection);
            outcome.status)
    {
    case ChecksumCorrectionOutcome::Status::kUnknownMcuType:
        // Unreachable: the find_flash_device precheck above returns first.
        // Handled defensively as a no-op, matching legacy's silent return.
        break;
    case ChecksumCorrectionOutcome::Status::kBadRomSize:
        showBadRomSizeDialog(parent);
        break;
    case ChecksumCorrectionOutcome::Status::kNoModuleForProtocol:
        if (selection.checksum_flag != "no")
        {
            result.canceled_due_to_missing_module = confirmProceedWithoutChecksumModule();
        }
        break;
    case ChecksumCorrectionOutcome::Status::kFamilyRan:
        if (outcome.family_result.has_value())
        {
            if (outcome.corrected_file.has_value())
            {
                result.corrected_rom_data = *outcome.corrected_file;
            }
            showFamilyResultDialog(*outcome.family_result);
        }
        break;
    }
    return result;
}

} // namespace fastecu::ui
