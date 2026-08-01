#include "src/ui/desktop/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_can.h"

#include <QMessageBox>

#include <utility>

#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/flash/eeprom/denso_sh705x_eeprom_can_executor.h"
#include "src/platform/desktop/common/flash/flash_snapshot_adapter.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/ports/qt_file_repository.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"

namespace
{

fastecu::flash::EepromReadMode nextMode(fastecu::flash::EepromReadMode mode)
{
    using fastecu::flash::EepromReadMode;
    switch (mode)
    {
    case EepromReadMode::Mode2:
        return EepromReadMode::Mode3;
    case EepromReadMode::Mode3:
    case EepromReadMode::Mode4:
        return EepromReadMode::Mode4;
    }
    return EepromReadMode::Mode4;
}

} // namespace

EepromEcuSubaruDensoSH705xCan::EepromEcuSubaruDensoSH705xCan(
    SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef,
    const fastecu::config::ConfigPaths& paths, const QString& cmd_type, QWidget *parent)
    : QDialog(parent), serial_(serial), ecuCalDef_(ecuCalDef), paths_(paths),
      cmd_type_(cmd_type),
      ui{std::make_unique<Ui::EcuOperationsWindow>()}
{
    ui->setupUi(this);

    if (cmd_type_ == "test_write")
    {
        this->setWindowTitle("Test write ROM " + ecuCalDef_->FileName + " to ECU");
    }
    else if (cmd_type_ == "write")
    {
        this->setWindowTitle("Write ROM " + ecuCalDef_->FileName + " to ECU");
    }
    else if (cmd_type_ == "read")
    {
        this->setWindowTitle("Read ROM from ECU");
    }
}

EepromEcuSubaruDensoSH705xCan::~EepromEcuSubaruDensoSH705xCan() = default;

void EepromEcuSubaruDensoSH705xCan::run()
{
    this->show();
    set_progressbar_value(0);

    if (cmd_type_ != "read")
    {
        // This dialog only ever builds an EEPROM *read* plan (buildPlan()
        // calls adapter.build_read_plan() unconditionally) -- MainWindow
        // dispatches this dialog by protocol name only, and cmd_type flows
        // through unfiltered from the generic top-level menu commands
        // (menu_actions.cpp's start_ecu_operations("write"/"test_write")),
        // so selecting "Write ROM to ECU" on this protocol is a real,
        // reachable path here, not a theoretical one. Without this guard it
        // would silently perform a real EEPROM read while the dialog is
        // titled "Write ROM ... to ECU", and a subsequent Save could
        // overwrite ecuCalDef_->FullRomData with the read bytes. Reject
        // before building any plan or starting a worker, matching the
        // InvalidConfig/Unsupported -> preflight message, no worker start
        // rule.
        //
        // Deliberately checked here rather than inside runAttempt(): the
        // very first runAttempt(Mode2) call below runs synchronously,
        // *before* the QEventLoop below has entered exec() -- and
        // QEventLoop::exec() unconditionally clears any pending quit()
        // requested before it starts (Qt resets its exit flag on entry), so
        // a close() (which requests loop_->quit() via closeEvent()) issued
        // from inside that first synchronous call is silently dropped and
        // loop.exec() blocks forever. Rejecting here, before the loop is
        // even constructed, avoids that hazard entirely -- exactly like the
        // ret != Ok decline path just below.
        showFailureDialog(fastecu::ErrorKind::Unsupported,
                          QString("cmd_type '%1' is not supported by this EEPROM read dialog")
                              .arg(cmd_type_));
        close();
        return;
    }

    const int ret = confirm(
        tr("Connecting to ECU"),
        tr("Downloading EEPROM content. There is 3 different option depends on "
           "ECU. All 3 option shows content on screen and you can save it when "
           "it looks ok.\n\n"
           "Turn ignition ON and press OK to start initializing connection to ECU"),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);

    if (ret != QMessageBox::Ok)
    {
        emit LOG_D("Operation canceled", true, true);
        close();
        return;
    }

    QEventLoop loop;
    loop_ = &loop;
    runAttempt(fastecu::flash::EepromReadMode::Mode2);
    loop.exec();
    loop_ = nullptr;
}

void EepromEcuSubaruDensoSH705xCan::runAttempt(fastecu::flash::EepromReadMode mode)
{
    currentMode_ = mode;

    fastecu::Result<fastecu::flash::FlashPlan> planResult = buildPlan(mode);
    if (!planResult.has_value())
    {
        // InvalidConfig/Unsupported: a static configuration problem, not a
        // per-attempt ECU response issue -- retrying at a different mode
        // would fail identically, so this stops the sequence immediately
        // without starting a FlashWorker.
        showFailureDialog(planResult.error().kind,
                          QString::fromStdString(planResult.error().detail));
        close();
        return;
    }

    worker_ = makeWorker(std::move(*planResult));

    connect(worker_.get(), &fastecu::flash::FlashWorker::logEvent, this,
            [this](int level, const QString& message)
            {
                switch (static_cast<fastecu::LogLevel>(level))
                {
                case fastecu::LogLevel::Error:
                    emit LOG_E(message, true, true);
                    break;
                case fastecu::LogLevel::Warning:
                    emit LOG_W(message, true, true);
                    break;
                case fastecu::LogLevel::Info:
                    emit LOG_I(message, true, true);
                    break;
                case fastecu::LogLevel::Debug:
                    emit LOG_D(message, true, true);
                    break;
                }
            });
    connect(worker_.get(), &fastecu::flash::FlashWorker::progressChanged, this,
            [this](int done, int total)
            {
                const int pct =
                    total > 0 ? static_cast<int>((static_cast<qint64>(done) * 100) / total) : done;
                set_progressbar_value(pct);
            });
    connect(worker_.get(), &fastecu::flash::FlashWorker::finished, this,
            &EepromEcuSubaruDensoSH705xCan::onWorkerFinished);

    worker_->start();
}

void EepromEcuSubaruDensoSH705xCan::onWorkerFinished(fastecu::flash::FlashWorkerResult result)
{
    // Join/destroy the just-finished worker before showing any modal
    // confirmation or starting the next attempt -- one bounded attempt at a
    // time, never overlapping.
    worker_.reset();

    if (!result.success)
    {
        if (result.error_kind == fastecu::ErrorKind::Cancelled)
        {
            close();
            return;
        }

        if (currentMode_ == fastecu::flash::EepromReadMode::Mode4)
        {
            showFailureDialog(result.error_kind, result.error_detail);
            close();
            return;
        }

        // A runtime (non-Cancelled) failure at mode 2 or 3 auto-advances to
        // the next mode without any dialog.
        runAttempt(nextMode(currentMode_));
        return;
    }

    const int reply =
        confirm(tr("Downloaded EEPROM content"),
                tr("If downloaded content looks ok, click 'Save' to save content and exit, "
                   "otherwise click 'discard' and continue with next method."),
                QMessageBox::Save | QMessageBox::Ignore, QMessageBox::Save);

    if (reply == QMessageBox::Save)
    {
        // Only Save updates ecuCalDef_->FullRomData.
        if (result.read_bytes.has_value())
        {
            ecuCalDef_->FullRomData =
                bytes::toQByteArray(bytes::ByteView(*result.read_bytes));
        }
        close();
        return;
    }

    // Ignore: discard this successful-but-rejected read and continue.
    if (currentMode_ == fastecu::flash::EepromReadMode::Mode4)
    {
        close();
        return;
    }

    const int ignitionReply =
        confirm(tr("Connecting to ECU"),
                tr("Turn ignition OFF and back ON and press OK to start initializing "
                   "connection to ECU"),
                QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
    if (ignitionReply != QMessageBox::Ok)
    {
        close();
        return;
    }

    runAttempt(nextMode(currentMode_));
}

fastecu::Result<fastecu::flash::FlashPlan> EepromEcuSubaruDensoSH705xCan::buildPlan(
    fastecu::flash::EepromReadMode mode)
{
    QtFileRepository file_repository;
    fastecu::flash::LegacyFlashSnapshotAdapter adapter(file_repository);
    return adapter.build_read_plan(*ecuCalDef_, ecuCalDef_->FlashMethod.toStdString(), mode,
                                   ecuCalDef_->Kernel.toStdString());
}

std::unique_ptr<fastecu::flash::FlashWorker> EepromEcuSubaruDensoSH705xCan::makeWorker(
    fastecu::flash::FlashPlan plan)
{
    return std::make_unique<fastecu::flash::FlashWorker>(
        std::move(plan), std::make_unique<fastecu::flash::DensoSh705xEepromCanExecutor>(),
        std::make_unique<fastecu::flash::DesktopCanFlashTransport>(serial_),
        std::make_unique<QtClock>());
}

int EepromEcuSubaruDensoSH705xCan::confirm(const QString& title, const QString& text,
                                           int buttons, int defaultButton)
{
    return QMessageBox::information(this, title, text,
                                    static_cast<QMessageBox::StandardButtons>(buttons),
                                    static_cast<QMessageBox::StandardButton>(defaultButton));
}

void EepromEcuSubaruDensoSH705xCan::showFailureDialog(fastecu::ErrorKind kind,
                                                      const QString& detail)
{
    using fastecu::ErrorKind;

    switch (kind)
    {
    case ErrorKind::InvalidConfig:
    case ErrorKind::Unsupported:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU flash configuration is invalid or unsupported for this "
                                "operation. Check the ROM definition and kernel file, then try "
                                "again."));
        break;
    case ErrorKind::Disconnected:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("Lost connection to the adapter or ECU. Check the cable/adapter "
                                "connection, press OK to exit and try again."));
        break;
    case ErrorKind::Timeout:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU did not respond in time, press OK to exit and try again."));
        break;
    case ErrorKind::BadResponse:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU returned an unexpected or rejected response, press OK to "
                                "exit and try again."));
        break;
    case ErrorKind::Cancelled:
        // Never reached: onWorkerFinished() short-circuits Cancelled before
        // calling showFailureDialog().
        break;
    case ErrorKind::Internal:
        // Verbatim legacy text, preserved exactly for this kind.
        QMessageBox::warning(this, tr("ECU Operation"),
                             "ECU operation failed, press OK to exit and try again");
        break;
    }

    emit LOG_E(QString("ECU operation failed (%1): %2")
                   .arg(QString::fromUtf8(fastecu::to_string(kind)), detail),
               true, true);
}

void EepromEcuSubaruDensoSH705xCan::closeEvent(QCloseEvent *event)
{
    if (worker_)
    {
        worker_->requestStop();
    }
    if (loop_)
    {
        loop_->quit();
    }
    QDialog::closeEvent(event);
}

void EepromEcuSubaruDensoSH705xCan::set_progressbar_value(int value)
{
    bool valueChanged = true;
    if (ui->progressbar)
    {
        valueChanged = ui->progressbar->value() != value;
        ui->progressbar->setValue(value);
    }
    if (valueChanged)
    {
        emit external_logger(value);
    }
}
