#include "src/ui/desktop/flash/common/flash_dialog.h"

#include <QCloseEvent>
#include <QMessageBox>

namespace fastecu::flash
{

FlashDialog::FlashDialog(std::unique_ptr<FlashWorkflow> workflow, FlashOperation operation, const QString& filename,
                         QWidget *parent)
    : QDialog(parent), workflow_(std::move(workflow)), ui_(std::make_unique<Ui::EcuOperationsWindow>())
{
    ui_->setupUi(this);
    if (operation == FlashOperation::Read)
    {
        setWindowTitle(tr("Read ROM from ECU"));
    }
    else if (operation == FlashOperation::TestWrite)
    {
        setWindowTitle(tr("Test write ROM %1 to ECU").arg(filename));
    }
    else
    {
        setWindowTitle(tr("Write ROM %1 to ECU").arg(filename));
    }
}

FlashDialogResult FlashDialog::run()
{
    show();
    setProgress(0, 100);
    QEventLoop loop;
    loop_ = &loop;
    advance();
    if (worker_)
    {
        loop.exec();
    }
    loop_ = nullptr;
    return std::move(result_);
}

void FlashDialog::advance()
{
    while (true)
    {
        FlashWorkflowStep step = workflow_->next();
        if (auto *prompt = std::get_if<FlashPromptStep>(&step))
        {
            workflow_->submit(presentPrompt(*prompt));
            continue;
        }
        if (auto *attempt = std::get_if<FlashAttempt>(&step))
        {
            startAttempt(std::move(*attempt));
            return;
        }
        if (auto *done = std::get_if<FlashCompletedStep>(&step))
        {
            result_ = {done->outcome, std::move(done->accepted_read_bytes), std::move(done->rom_id)};
            if (done->outcome == FlashWorkflowOutcome::Succeeded)
            {
                showSuccess();
            }
            close();
            if (loop_)
            {
                loop_->quit();
            }
            return;
        }
        const Error error = std::move(std::get<FlashFailureStep>(step).error);
        result_.outcome =
            error.kind == ErrorKind::Cancelled ? FlashWorkflowOutcome::Cancelled : FlashWorkflowOutcome::Failed;
        if (error.kind != ErrorKind::Cancelled)
        {
            showFailure(error);
        }
        close();
        if (loop_)
        {
            loop_->quit();
        }
        return;
    }
}

void FlashDialog::startAttempt(FlashAttempt attempt)
{
    worker_ = std::make_unique<FlashWorker>(std::move(attempt.plan), std::move(attempt.executor),
                                            std::move(attempt.transport), std::move(attempt.clock));
    connect(worker_.get(), &FlashWorker::logEvent, this,
            [this](int level, const QString& message)
            {
                using enum LogLevel;
                switch (static_cast<LogLevel>(level))
                {
                case Error:
                    emit LOG_E(message, true, true);
                    break;
                case Warning:
                    emit LOG_W(message, true, true);
                    break;
                case Info:
                    emit LOG_I(message, true, true);
                    break;
                case Debug:
                    emit LOG_D(message, true, true);
                    break;
                }
            });
    connect(worker_.get(), &FlashWorker::progressChanged, this, &FlashDialog::setProgress);
    connect(worker_.get(), &FlashWorker::phaseProgressChanged, this,
            [this](const QString& phase, int index, int count, int done, int total)
            {
                if (ui_->progressbar)
                {
                    ui_->progressbar->setFormat(tr("Phase %1/%2 - %3: %p%").arg(index).arg(count).arg(phase));
                }
                setProgress(done, total);
            });
    connect(worker_.get(), &FlashWorker::finished, this, &FlashDialog::workerFinished);
    worker_->start();
}

void FlashDialog::workerFinished(FlashWorkerResult result)
{
    worker_.reset();
    emit external_logger("Finished");
    workflow_->submit(FlashAttemptResult{result.success, result.error_kind, result.error_detail.toStdString(),
                                         std::move(result.read_bytes), std::move(result.rom_id)});
    advance();
}

FlashPromptResponse FlashDialog::presentPrompt(const FlashPromptStep& prompt)
{
    auto arg = [&prompt](const char *key)
    {
        for (const auto& [name, value] : prompt.arguments)
        {
            if (name == key)
            {
                return QString::fromStdString(value);
            }
        }
        return QString{};
    };
    if (prompt.kind == FlashPromptKind::ColtEraseTrigger)
    {
        const QString text =
            tr("This operation accepts an exact %1 KiB ROM. The file's first 32 KiB (0x0000-%2) will be ignored; only "
               "%2-%3 is writable, so the ECU bootloader will remain unchanged.\n\nAbout to send the flash-erase "
               "trigger command. This exact sequence is known to have locked up the bootloader during the original "
               "implementation's testing. Only continue if this is a bench/spare ECU with a recovery path available. "
               "Cancellation after erase can leave an incomplete image requiring recovery.\n\nContinue?")
                .arg(arg("capacity_kib"), arg("writable_start_hex"), arg("rom_end_hex"));
        return QMessageBox::warning(this, tr("Erase trigger"), text, QMessageBox::Yes | QMessageBox::Cancel,
                                    QMessageBox::Cancel) == QMessageBox::Yes
                   ? FlashPromptResponse::Accept
                   : FlashPromptResponse::Decline;
    }
    if (prompt.kind == FlashPromptKind::ColtTopRegionBootstrap)
    {
        const QString text =
            tr("The top 128KB (%1-%2) may not match the ROM being written. If it does not, it needs a one-time "
               "bootstrap pass through custom erase/write redirect helpers, outside the range the vendor bootloader "
               "normally allows. This sends the same high-risk erase trigger sequence used for the main write, once "
               "then and once more for the main write that follows. Only continue on a bench/spare ECU with a recovery "
               "path available.\n\nContinue?")
                .arg(arg("top_region_start_hex"), arg("rom_end_hex"));
        return QMessageBox::warning(this, tr("Top 128KB bootstrap"), text, QMessageBox::Yes | QMessageBox::Cancel,
                                    QMessageBox::Cancel) == QMessageBox::Yes
                   ? FlashPromptResponse::Accept
                   : FlashPromptResponse::Decline;
    }
    if (prompt.kind == FlashPromptKind::InspectRead)
    {
        return QMessageBox::information(this, tr("Downloaded EEPROM content"),
                                        tr("If downloaded content looks correct, click Save to accept it and exit; "
                                           "otherwise click Discard to continue with the next method."),
                                        QMessageBox::Save | QMessageBox::Discard,
                                        QMessageBox::Save) == QMessageBox::Save
                   ? FlashPromptResponse::Save
                   : FlashPromptResponse::Discard;
    }
    const bool cycle = prompt.kind == FlashPromptKind::CycleIgnition;
    const QString text = cycle ? tr("Turn ignition OFF and back ON, then press OK to continue.")
                               : tr("Turn ignition ON and press OK to start initializing the ECU connection.");
    return QMessageBox::information(this, tr("Connecting to ECU"), text, QMessageBox::Ok | QMessageBox::Cancel,
                                    QMessageBox::Cancel) == QMessageBox::Ok
               ? FlashPromptResponse::Accept
               : FlashPromptResponse::Decline;
}

void FlashDialog::showSuccess()
{
    QMessageBox::information(this, tr("ECU Operation"), tr("ECU operation completed successfully. Press OK to exit."));
}

void FlashDialog::showFailure(const Error& error)
{
    using enum ErrorKind;
    QString text;
    switch (error.kind)
    {
    case InvalidConfig:
    case Unsupported:
        text = tr("ECU flash configuration is invalid or unsupported. Check the selected protocol, ROM definition, and "
                  "kernel file.");
        break;
    case Disconnected:
        text = tr("Lost connection to the adapter or ECU. Check the connection and try again.");
        break;
    case Timeout:
        text = tr("ECU did not respond in time. Check the connection and try again.");
        break;
    case BadResponse:
        text = tr("ECU returned an unexpected or rejected response. Check the setup and try again.");
        break;
    case Internal:
        text = tr("ECU operation failed. Press OK to exit and try again.");
        break;
    case Cancelled:
        return;
    }
    QMessageBox::warning(this, tr("ECU Operation"), text);
    emit LOG_E(QString("ECU operation failed (%1): %2")
                   .arg(QString::fromUtf8(to_string(error.kind)), QString::fromStdString(error.detail)),
               true, true);
}

void FlashDialog::closeEvent(QCloseEvent *event)
{
    if (worker_)
    {
        worker_->requestStop();
        worker_.reset();
        result_.outcome = FlashWorkflowOutcome::Cancelled;
    }
    if (loop_)
    {
        loop_->quit();
    }
    QDialog::closeEvent(event);
}

void FlashDialog::setProgress(int done, int total)
{
    const int value = total > 0 ? int((qint64(done) * 100) / total) : done;
    if (!ui_->progressbar || ui_->progressbar->value() != value)
    {
        if (ui_->progressbar)
        {
            ui_->progressbar->setValue(value);
        }
        emit external_logger(value);
    }
}

} // namespace fastecu::flash
