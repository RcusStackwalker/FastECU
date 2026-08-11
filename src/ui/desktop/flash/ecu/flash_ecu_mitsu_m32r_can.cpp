#include "src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h"

#include <QCloseEvent>
#include <QMessageBox>

#include <algorithm>
#include <optional>
#include <ranges>
#include <utility>

#include "src/algorithms/protocol/qt_bytes.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_executor.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/platform/desktop/common/ports/qt_clock.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"

FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan(SerialPortActions *serial, FileActions::EcuCalDefStructure *ecuCalDef, const QString& cmd_type, QWidget *parent)
    : QDialog(parent), ecuCalDef(ecuCalDef), cmd_type(cmd_type), serial(serial), ui{std::make_unique<Ui::EcuOperationsWindow>()}
{
    ui->setupUi(this);

    if (cmd_type == "write")
    {
        this->setWindowTitle("Write ROM " + ecuCalDef->FileName + " to ECU");
    }
    else if (cmd_type == "read")
    {
        this->setWindowTitle("Read ROM from ECU");
    }
}

FlashEcuMitsuM32rCan::~FlashEcuMitsuM32rCan()
{
}

void FlashEcuMitsuM32rCan::run()
{
    this->show();
    set_progressbar_value(0);

    // Preflight must finish before the operator sees any confirmation and
    // before a worker or transport exists. In particular, an incorrectly
    // sized ROM cannot get as far as a high-risk erase prompt.
    fastecu::Result<fastecu::flash::FlashPlan> planResult = buildPlan();
    if (!planResult.has_value())
    {
        showFailureDialog(planResult.error().kind,
                          QString::fromStdString(planResult.error().detail));
        close();
        return;
    }

    fastecu::flash::FlashPlan& plan = *planResult;
    const auto requiresConfirmation = [&plan](fastecu::flash::ConfirmationSpec::Id id)
    {
        return std::ranges::any_of(
            plan.confirmations(),
            [id](const fastecu::flash::ConfirmationSpec& spec)
            { return spec.id == id; });
    };

    const int ret = confirm(
        tr("Connecting to ECU"),
        tr("Turn ignition ON and press OK to start initializing connection to ECU"),
        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
    if (ret != QMessageBox::Ok)
    {
        emit LOG_D("Operation canceled", true, true);
        close();
        return;
    }

    if (requiresConfirmation(fastecu::flash::ConfirmationSpec::Id::EraseTrigger))
    {
        const std::uint32_t romEndValue =
            plan.transfer_region().start + plan.transfer_region().length;
        const QString capacityKiB = QString::number(romEndValue / 1024);
        const QString romEnd = QString::number(romEndValue, 16);
        const int eraseReply =
            confirm(tr("Erase trigger"),
                    tr("This operation accepts an exact %1 KiB ROM. The file's first "
                       "32 KiB (0x0000-0x8000) will be ignored; only "
                       "0x8000-0x%2 is writable, so the ECU bootloader will remain "
                       "unchanged.\n\nAbout to send the flash-erase trigger command. This exact "
                       "sequence is known to have locked up the bootloader during the "
                       "original implementation's testing. Only continue if this is a "
                       "bench/spare ECU with a recovery path available. Cancellation after "
                       "erase can leave an incomplete image requiring recovery.\n\nContinue?")
                        .arg(capacityKiB, romEnd),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (eraseReply != QMessageBox::Yes)
        {
            emit LOG_I("Erase trigger canceled by user", true, true);
            close();
            return;
        }
    }

    if (requiresConfirmation(fastecu::flash::ConfirmationSpec::Id::TopRegionBootstrap))
    {
        const int bootstrapReply =
            confirm(tr("Top 128KB bootstrap"),
                    tr("The top 128KB (0x60000-0x80000) may not match the ROM being "
                       "written. If it does not, it needs a one-time bootstrap pass "
                       "through custom erase/write redirect helpers, outside the range "
                       "the vendor bootloader normally allows. This sends the same "
                       "high-risk erase trigger sequence used for the main write, once "
                       "then and once more for the main write that follows. Only "
                       "continue on a bench/spare ECU with a recovery path available."
                       "\n\nContinue?"),
                    QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (bootstrapReply != QMessageBox::Yes)
        {
            emit LOG_I("Top 128KB bootstrap canceled by user", true, true);
            close();
            return;
        }
    }

    worker_ = makeWorker(std::move(plan));

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
    connect(worker_.get(), &fastecu::flash::FlashWorker::phaseProgressChanged, this,
            [this](const QString& phaseName, int phaseIndex, int phaseCount, int done, int total)
            {
                const int pct =
                    total > 0 ? static_cast<int>((static_cast<qint64>(done) * 100) / total)
                              : done;
                if (ui->progressbar)
                {
                    ui->progressbar->setFormat(
                        tr("Phase %1/%2 - %3: %p%").arg(phaseIndex).arg(phaseCount).arg(phaseName));
                }
                set_progressbar_value(pct);
            });
    connect(worker_.get(), &fastecu::flash::FlashWorker::finished, this,
            &FlashEcuMitsuM32rCan::onWorkerFinished);

    QEventLoop loop;
    loop_ = &loop;
    worker_->start();
    loop.exec();
    loop_ = nullptr;
}

void FlashEcuMitsuM32rCan::onWorkerFinished(fastecu::flash::FlashWorkerResult result)
{
    // Join/destroy the worker (and with it the transport adapter) before any
    // modal dialog runs, so exactly one bounded attempt is ever in flight.
    worker_.reset();
    emit external_logger("Finished");

    if (result.success)
    {
        if (result.read_bytes.has_value())
        {
            ecuCalDef->FullRomData =
                bytes::toQByteArray(bytes::ByteView(*result.read_bytes));
        }
        showSuccessDialog();
        close();
        return;
    }

    if (result.error_kind == fastecu::ErrorKind::Cancelled)
    {
        close();
        return;
    }

    showFailureDialog(result.error_kind, result.error_detail);
    if (loop_)
    {
        loop_->quit();
    }
}

fastecu::Result<fastecu::flash::FlashPlan> FlashEcuMitsuM32rCan::buildPlan()
{
    using fastecu::flash::FlashOperation;

    FlashOperation operation = FlashOperation::Read;
    std::optional<bytes::Bytes> image;
    if (cmd_type == "write")
    {
        operation = FlashOperation::Write;
        image = bytes::fromQByteArray(ecuCalDef->FullRomData);
    }
    else if (cmd_type == "test_write")
    {
        operation = FlashOperation::TestWrite;
    }

    return fastecu::flash::build_mitsu_colt_m32r_can_plan(
        operation, ecuCalDef->FlashMethod.toStdString(), ecuCalDef->McuType.toStdString(),
        std::move(image));
}

std::unique_ptr<fastecu::flash::FlashWorker> FlashEcuMitsuM32rCan::makeWorker(
    fastecu::flash::FlashPlan plan)
{
    // Port lifetime for this family, made explicit because the two CAN
    // executors carry opposite close() contracts:
    //
    //  - MitsuColtM32rCanExecutor configures and opens the transport and
    //    deliberately never closes it (legacy-faithful: no legacy Mitsu Colt
    //    flash op called close_serial_port).
    //  - DensoSh705xEepromCanExecutor, driven by the sibling EEPROM dialogs,
    //    closes on every exit path via a ScopedClose guard.
    //
    // Both dialogs pass MainWindow's single session-lifetime
    // SerialPortActions through DesktopCanFlashTransport's NON-OWNING
    // constructor, and on that constructor close() only detaches the
    // adapter's own pointer -- it never destroys or closes the underlying
    // SerialPortActions (desktop_can_flash_transport.cpp:119-126). So the two
    // contracts are observationally identical from here: the port MainWindow
    // owns stays open across runs either way, and the next operation's
    // open() re-opens it.
    //
    // Consequently this dialog deliberately does nothing about the port on
    // any exit path. The only per-run teardown is the transport adapter
    // itself, destroyed with the worker in onWorkerFinished()/closeEvent().
    return std::make_unique<fastecu::flash::FlashWorker>(
        std::move(plan), std::make_unique<fastecu::flash::MitsuColtM32rCanExecutor>(),
        std::make_unique<fastecu::flash::DesktopCanFlashTransport>(serial),
        std::make_unique<QtClock>());
}

int FlashEcuMitsuM32rCan::confirm(const QString& title, const QString& text, int buttons,
                                  int defaultButton)
{
    return QMessageBox::warning(this, title, text,
                                static_cast<QMessageBox::StandardButtons>(buttons),
                                static_cast<QMessageBox::StandardButton>(defaultButton));
}

void FlashEcuMitsuM32rCan::showSuccessDialog()
{
    // Verbatim legacy text, including the original's spelling of "succesful".
    QMessageBox::information(this, tr("ECU Operation"),
                             "ECU operation was succesful, press OK to exit");
}

void FlashEcuMitsuM32rCan::showFailureDialog(fastecu::ErrorKind kind, const QString& detail)
{
    using fastecu::ErrorKind;

    switch (kind)
    {
    case ErrorKind::InvalidConfig:
    case ErrorKind::Unsupported:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU flash configuration is invalid or unsupported for this "
                                "operation. Check the ROM definition and selected protocol, "
                                "then try again."));
        break;
    case ErrorKind::Disconnected:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("Lost connection to the adapter or ECU. Check the "
                                "cable/adapter connection, press OK to exit and try again."));
        break;
    case ErrorKind::Timeout:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU did not respond in time, press OK to exit and try "
                                "again."));
        break;
    case ErrorKind::BadResponse:
        QMessageBox::warning(this, tr("ECU Operation"),
                             tr("ECU returned an unexpected or rejected response, press OK "
                                "to exit and try again."));
        break;
    case ErrorKind::Cancelled:
        // Never reached: onWorkerFinished() short-circuits Cancelled before
        // calling showFailureDialog().
        break;
    case ErrorKind::Internal:
        // Verbatim legacy text -- the only failure message the pre-rewrite
        // dialog ever showed.
        QMessageBox::warning(this, tr("ECU Operation"),
                             "ECU operation failed, press OK to exit and try again");
        break;
    }

    emit LOG_E(QString("ECU operation failed (%1): %2")
                   .arg(QString::fromUtf8(fastecu::to_string(kind)), detail),
               true, true);
}

void FlashEcuMitsuM32rCan::closeEvent(QCloseEvent *event)
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

void FlashEcuMitsuM32rCan::set_progressbar_value(int value)
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
