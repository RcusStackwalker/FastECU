#include "src/ui/desktop/flash/common/flash_dialog.h"

#include <QtTest>

namespace fastecu::flash
{
namespace
{

class ScriptedWorkflow final : public FlashWorkflow
{
  public:
    FlashWorkflowStep next() override
    {
        if (!answered_)
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        return FlashCompletedStep{FlashWorkflowOutcome::Succeeded, bytes::Bytes{0x12, 0x34}};
    }
    void submit(FlashPromptResponse response) override
    {
        answered_ = response == FlashPromptResponse::Accept;
    }
    void submit(FlashAttemptResult) override
    {
    }

  private:
    bool answered_ = false;
};

class RecordingDialog final : public FlashDialog
{
  public:
    RecordingDialog(std::unique_ptr<FlashWorkflow> workflow, FlashOperation operation,
                    const QString& filename)
        : FlashDialog(std::move(workflow), operation, filename)
    {
    }
    QList<FlashPromptKind> prompts;
    bool success_shown = false;

  protected:
    FlashPromptResponse presentPrompt(const FlashPromptStep& prompt) override
    {
        prompts.push_back(prompt.kind);
        return FlashPromptResponse::Accept;
    }
    void showSuccess() override
    {
        success_shown = true;
    }
    void showFailure(const Error&) override
    {
        QFAIL("unexpected failure");
    }
};

class FlashDialogTest : public QObject
{
    Q_OBJECT
  private slots:
    void returnsAcceptedBytesAndUsesNormalizedReadTitle()
    {
        RecordingDialog dialog(std::make_unique<ScriptedWorkflow>(), FlashOperation::Read,
                               "ignored.bin");
        const FlashDialogResult result = dialog.run();
        QCOMPARE(dialog.windowTitle(), QString("Read ROM from ECU"));
        QCOMPARE(dialog.prompts, QList{FlashPromptKind::Begin});
        QVERIFY(dialog.success_shown);
        QCOMPARE(result.outcome, FlashWorkflowOutcome::Succeeded);
        QCOMPARE(result.accepted_read_bytes, bytes::Bytes({0x12, 0x34}));
    }
};

} // namespace
} // namespace fastecu::flash

QTEST_MAIN(fastecu::flash::FlashDialogTest)
#include "flash_dialog_test.moc"
