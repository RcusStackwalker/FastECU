#include "src/ui/desktop/flash/common/flash_dialog.h"

#include <QtTest>

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
#include "src/backend/ports/testing/fake_clock.h"

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
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        return FlashCompletedStep{FlashWorkflowOutcome::Succeeded, bytes::Bytes{0x12, 0x34},
                                  std::string("123456789A_")};
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

// Runs on the FlashWorker thread and blocks like a transport mid-read until
// the dialog's requestStop() unblocks it, then reports the cancellation.
class BlockingAttempt final : public BoundFlashAttempt
{
  public:
    explicit BlockingAttempt(FlashPlan plan) : plan_(std::move(plan))
    {
    }
    const FlashPlan& plan() const noexcept override
    {
        return plan_;
    }
    Result<FlashExecutionResult> run(IClock&, const ICancellationToken&, IEventSink&) override
    {
        std::unique_lock lock(mutex_);
        started_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return unblocked_; });
        return fail(ErrorKind::Cancelled, "unblocked");
    }
    void request_unblock() noexcept override
    {
        const std::scoped_lock lock(mutex_);
        unblocked_ = true;
        changed_.notify_all();
    }
    bool waitUntilStarted()
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds(5), [this] { return started_; });
    }

  private:
    FlashPlan plan_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool started_ = false;
    bool unblocked_ = false;
};

// Begin -> blocking attempt; a cancelled attempt yields the post-attempt
// notice, then completes as Cancelled.
class CancellableWorkflow final : public FlashWorkflow
{
  public:
    FlashWorkflowStep next() override
    {
        if (!begun_)
        {
            return FlashPromptStep{FlashPromptKind::Begin, {}};
        }
        if (!attempted_)
        {
            attempted_ = true;
            auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::Read, "sub_ecu_unisia_jecs_20",
                                                                 "M32R_128KB", std::nullopt, true);
            if (!plan.has_value())
            {
                return FlashFailureStep{plan.error()};
            }
            auto attempt = std::make_unique<BlockingAttempt>(std::move(*plan));
            attempt_ = attempt.get();
            return FlashAttempt{std::move(attempt), std::make_unique<FakeClock>()};
        }
        if (notice_due_)
        {
            return FlashPromptStep{FlashPromptKind::RemoveProgrammingVoltage,
                                   {{"outcome", "cancelled"}, {"external_vpp", "yes"}}};
        }
        return FlashCompletedStep{FlashWorkflowOutcome::Cancelled, std::nullopt, std::nullopt};
    }
    void submit(FlashPromptResponse) override
    {
        if (begun_)
        {
            notice_due_ = false;
        }
        begun_ = true;
    }
    void submit(FlashAttemptResult result) override
    {
        attempt_results.push_back(result.error_kind);
        notice_due_ = !result.success && result.error_kind == ErrorKind::Cancelled;
    }

    BlockingAttempt *attempt_ = nullptr; // owned by the FlashWorker once started
    QList<ErrorKind> attempt_results;

  private:
    bool begun_ = false;
    bool attempted_ = false;
    bool notice_due_ = false;
};

class RecordingDialog final : public FlashDialog
{
  public:
    RecordingDialog(std::unique_ptr<FlashWorkflow> workflow, FlashOperation operation, const QString& filename)
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
        RecordingDialog dialog(std::make_unique<ScriptedWorkflow>(), FlashOperation::Read, "ignored.bin");
        const FlashDialogResult result = dialog.run();
        QCOMPARE(dialog.windowTitle(), QString("Read ROM from ECU"));
        QCOMPARE(dialog.prompts, QList{FlashPromptKind::Begin});
        QVERIFY(dialog.success_shown);
        QCOMPARE(result.outcome, FlashWorkflowOutcome::Succeeded);
        QCOMPARE(result.accepted_read_bytes, bytes::Bytes({0x12, 0x34}));
        QCOMPARE(result.rom_id, std::string("123456789A_"));
    }

    // Closing the dialog is the only cancel path in the app: the workflow must
    // hear the cancelled attempt and get to present its post-attempt notice.
    void closingMidAttemptSubmitsCancelledAndPresentsTheNotice()
    {
        auto owned = std::make_unique<CancellableWorkflow>();
        CancellableWorkflow *workflow = owned.get();
        RecordingDialog dialog(std::move(owned), FlashOperation::Write, "rom.bin");
        QTimer::singleShot(0, &dialog,
                           [&dialog, workflow]
                           {
                               QVERIFY(workflow->attempt_ != nullptr);
                               QVERIFY(workflow->attempt_->waitUntilStarted());
                               dialog.close();
                           });
        const FlashDialogResult result = dialog.run();
        QCOMPARE(result.outcome, FlashWorkflowOutcome::Cancelled);
        QCOMPARE(dialog.prompts, (QList{FlashPromptKind::Begin, FlashPromptKind::RemoveProgrammingVoltage}));
        QCOMPARE(workflow->attempt_results, QList{ErrorKind::Cancelled});
        QVERIFY(!dialog.success_shown);

        // The worker emitted finished before closeEvent joined it; that queued
        // delivery must not submit the attempt a second time.
        QCoreApplication::sendPostedEvents();
        QCoreApplication::processEvents();
        QCOMPARE(workflow->attempt_results, QList{ErrorKind::Cancelled});
        QCOMPARE(dialog.prompts, (QList{FlashPromptKind::Begin, FlashPromptKind::RemoveProgrammingVoltage}));
        QVERIFY(!dialog.success_shown);
    }
};

} // namespace
} // namespace fastecu::flash

QTEST_MAIN(fastecu::flash::FlashDialogTest)
#include "flash_dialog_test.moc"
