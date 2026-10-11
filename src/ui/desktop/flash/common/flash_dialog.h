#pragma once

#include <memory>
#include <optional>

#include <QDialog>
#include <QEventLoop>

#include "src/platform/desktop/common/flash/flash_workflow.h"
#include "src/platform/desktop/common/flash/worker/flash_worker.h"
#include <ui_ecu_operations.h>

namespace fastecu::flash
{

struct FlashDialogResult
{
    FlashWorkflowOutcome outcome = FlashWorkflowOutcome::kFailed;
    std::optional<bytes::Bytes> accepted_read_bytes;
    std::optional<std::string> rom_id;
};

struct ProgrammingVoltageNotice
{
    QString title;
    QString text;
};

class FlashDialog : public QDialog
{
    Q_OBJECT
  public:
    explicit FlashDialog(std::unique_ptr<FlashWorkflow> workflow, FlashOperation operation, const QString& filename,
                         QWidget *parent = nullptr);
    ~FlashDialog() override = default;
    FlashDialogResult run();

    // The RemoveProgrammingVoltage notice's title and text; see the prompt
    // kind's arguments in flash_workflow.h.
    static ProgrammingVoltageNotice programmingVoltageNotice(const FlashPromptStep& prompt);

  signals:
    void externalLogger(QString message);
    void externalLogger(int value);
    void logE(QString message, bool timestamp, bool linefeed);
    void logW(QString message, bool timestamp, bool linefeed);
    void logI(QString message, bool timestamp, bool linefeed);
    void logD(QString message, bool timestamp, bool linefeed);

  protected:
    virtual FlashPromptResponse presentPrompt(const FlashPromptStep& prompt);
    virtual void showSuccess();
    virtual void showFailure(const Error& error);
    void closeEvent(QCloseEvent *event) override;

  private:
    void advance();
    void startAttempt(FlashAttempt attempt);
    void workerFinished(FlashWorkerResult result);
    void finishCancelledAttempt();
    void setProgress(int done, int total);

    std::unique_ptr<FlashWorkflow> workflow_;
    std::unique_ptr<FlashWorker> worker_;
    QEventLoop *loop_ = nullptr;
    FlashDialogResult result_;
    std::unique_ptr<Ui::EcuOperationsWindow> ui_;
};

} // namespace fastecu::flash
