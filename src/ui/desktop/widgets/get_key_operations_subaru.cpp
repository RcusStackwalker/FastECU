#include "src/ui/desktop/widgets/get_key_operations_subaru.h"
#include <ui_ecu_operations.h>
#include "src/algorithms/crypto/subaru_key_recovery.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"

namespace
{
// Return codes of this dialog's own helpers.
constexpr int kStatusSuccess = 0x00;
constexpr int kStatusError = 0x01;
} // namespace

GetKeyOperationsSubaru::GetKeyOperationsSubaru(QWidget *parent)
    : QDialog(parent), ui_{std::make_unique<Ui::EcuOperationsWindow>()}
{
    ui_->setupUi(this);

    this->setWindowTitle("Determine Encryption Keys from Unencrypted and Encrypted Files");
    this->show();

    int result = 0;

    ui_->progressbar->setValue(0);

    result = loadAndApplyLinearApprox();

    if (result == kStatusSuccess)
    {
        QMessageBox::information(this, tr("Get Key Operation"),
                                 "Get Key operation completed succesfully, press OK to exit");
        this->accept();
        // this->close();
    }
    else
    {
        QMessageBox::warning(this, tr("Get Key Operation"), "Get Key operation failed, press OK to exit and try again");
    }
}

GetKeyOperationsSubaru::~GetKeyOperationsSubaru()
{
}

void GetKeyOperationsSubaru::closeEvent(QCloseEvent *bar)
{
    // kill_process = true;
}

int GetKeyOperationsSubaru::loadAndApplyLinearApprox()
{
    // QFileDialog openDialog;
    QString unencryptedFilename = QFileDialog::getOpenFileName(this, tr("Select unencrypted ROM"));

    if (unencryptedFilename.isEmpty())
    {
        QMessageBox::information(this, tr("Unencrypted file"), "No file selected");
    }

    QString encryptedFilename = QFileDialog::getOpenFileName(this, tr("Select encrypted ROM"));

    if (encryptedFilename.isEmpty())
    {
        QMessageBox::information(this, tr("Encrypted file"), "No file selected");
    }

    QFile unencryptedFile(unencryptedFilename);
    if (!unencryptedFile.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("File"), "Unable to open file for reading");
        return kStatusError;
    }
    QByteArray unencryptedFileData = unencryptedFile.readAll();
    unencryptedFile.close();

    QFile encryptedFile(encryptedFilename);
    if (!encryptedFile.open(QIODevice::ReadOnly))
    {
        QMessageBox::warning(this, tr("File"), "Unable to open file for reading");
        return kStatusError;
    }
    QByteArray encryptedFileData = encryptedFile.readAll();
    encryptedFile.close();

    emit logI("Files loaded successfully", true, true);

    emit logI("Start Time", true, true);
    const auto recovery =
        subaru_key_recovery::RecoverKeys(bytes::View(unencryptedFileData), bytes::View(encryptedFileData));
    if (!recovery.has_value())
    {
        QMessageBox::warning(this, tr("Get Key Operation"),
                             recovery.error() == subaru_key_recovery::Failure::kInputTooShort
                                 ? "Both ROM files must be at least 128 KiB"
                                 : "No key is consistent with most of these ROM files' words");
        return kStatusError;
    }
    const auto& [k1, k2, k3, k4] = recovery->keys;

    emit logI("Predicted k4: 0x" + QString::number(k4, 16), true, true);
    emit logI("Moving on to k1", true, true);
    emit logI("Predicted k1: 0x" + QString::number(k1, 16), true, true);
    emit logI("Moving on to k2", true, true);
    emit logI("Predicted k2: 0x" + QString::number(k2, 16), true, true);
    emit logI("Moving on to k3", true, true);
    emit logI("Predicted k3: 0x" + QString::number(k3, 16), true, true);
    emit logI("Keys reproduce " + QString::number(recovery->reproduced_pairs) + " of " +
                  QString::number(recovery->distinct_pairs) + " distinct word pairs",
              true, true);
    emit logI("End Time", true, true);

    return kStatusSuccess;
}
