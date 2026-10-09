#include <QTimer>
#include "src/platform/desktop/common/testing/widgets_application_environment.h"
#include "src/ui/desktop/flash/common/flash_dialog.h"

#include <gtest/gtest.h>

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
            return FlashPromptStep{FlashPromptKind::kBegin, {}};
        }
        return FlashCompletedStep{FlashWorkflowOutcome::kSucceeded, bytes::Bytes{0x12, 0x34},
                                  std::string("123456789A_")};
    }
    void submit(FlashPromptResponse response) override
    {
        answered_ = response == FlashPromptResponse::kAccept;
    }
    void submit(FlashAttemptResult) override
    {
    }

  private:
    bool answered_ = false;
};

// Completes at once; lets a dialog test run a workflow with several attempts.
class InstantAttempt final : public BoundFlashAttempt
{
  public:
    explicit InstantAttempt(FlashPlan plan) : plan_(std::move(plan))
    {
    }
    const FlashPlan& plan() const noexcept override
    {
        return plan_;
    }
    Result<FlashExecutionResult> run(IClock&, const ICancellationToken&, IEventSink&) override
    {
        return FlashExecutionResult{
            .operation = FlashOperation::kWrite, .read_bytes = std::nullopt, .rom_id = std::nullopt};
    }
    void request_unblock() noexcept override
    {
    }

  private:
    FlashPlan plan_;
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
        return fail(ErrorKind::kCancelled, "unblocked");
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
            return FlashPromptStep{FlashPromptKind::kBegin, {}};
        }
        if (!attempted_)
        {
            attempted_ = true;
            auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::kRead, "sub_ecu_unisia_jecs_20",
                                                                 "M32R_128KB", std::nullopt, true);
            if (!plan.has_value())
            {
                return FlashFailureStep{plan.error()};
            }
            auto attempt = std::make_unique<BlockingAttempt>(std::move(*plan));
            active_attempt = attempt.get();
            return FlashAttempt{std::move(attempt), std::make_unique<FakeClock>()};
        }
        if (notice_due_)
        {
            return FlashPromptStep{FlashPromptKind::kRemoveProgrammingVoltage,
                                   {{"outcome", "cancelled"}, {"external_vpp", "yes"}}};
        }
        return FlashCompletedStep{FlashWorkflowOutcome::kCancelled, std::nullopt, std::nullopt};
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
        notice_due_ = !result.success && result.error_kind == ErrorKind::kCancelled;
    }

    BlockingAttempt *active_attempt = nullptr; // owned by the FlashWorker once started
    QList<ErrorKind> attempt_results;

  private:
    bool begun_ = false;
    bool attempted_ = false;
    bool notice_due_ = false;
};

// Begin -> attempt -> RemoveMod1 -> attempt -> completed: the bootmode shape.
class TwoAttemptWorkflow final : public FlashWorkflow
{
  public:
    FlashWorkflowStep next() override
    {
        if (step_ == 0)
        {
            return FlashPromptStep{FlashPromptKind::kBegin, {}};
        }
        if (step_ == 2)
        {
            return FlashPromptStep{FlashPromptKind::kRemoveMod1, {}};
        }
        if (step_ == 1 || step_ == 3)
        {
            ++step_;
            auto plan = build_subaru_unisia_jecs_m32r_kline_plan(FlashOperation::kRead, "sub_ecu_unisia_jecs_20",
                                                                 "M32R_128KB", std::nullopt, true);
            if (!plan.has_value())
            {
                return FlashFailureStep{plan.error()};
            }
            return FlashAttempt{std::make_unique<InstantAttempt>(std::move(*plan)), std::make_unique<FakeClock>()};
        }
        return FlashCompletedStep{FlashWorkflowOutcome::kSucceeded, std::nullopt, std::nullopt};
    }
    void submit(FlashPromptResponse) override
    {
        ++step_;
    }
    void submit(FlashAttemptResult result) override
    {
        attempts.push_back(result.success);
    }

    QList<bool> attempts;

  private:
    int step_ = 0;
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
        return FlashPromptResponse::kAccept;
    }
    void showSuccess() override
    {
        success_shown = true;
    }
    void showFailure(const Error&) override
    {
        FAIL() << "unexpected failure";
    }
};

TEST(FlashDialogTest, returnsAcceptedBytesAndUsesNormalizedReadTitle)
{
    RecordingDialog dialog(std::make_unique<ScriptedWorkflow>(), FlashOperation::kRead, "ignored.bin");
    const FlashDialogResult result = dialog.run();
    ASSERT_EQ(dialog.windowTitle(), QString("Read ROM from ECU"));
    ASSERT_EQ(dialog.prompts, QList{FlashPromptKind::kBegin});
    ASSERT_TRUE(dialog.success_shown);
    ASSERT_EQ(result.outcome, FlashWorkflowOutcome::kSucceeded);
    ASSERT_EQ(result.accepted_read_bytes, bytes::Bytes({0x12, 0x34}));
    ASSERT_EQ(result.rom_id, std::string("123456789A_"));
}

// Closing the dialog is the only cancel path in the app: the workflow must
// hear the cancelled attempt and get to present its post-attempt notice.
TEST(FlashDialogTest, closingMidAttemptSubmitsCancelledAndPresentsTheNotice)
{
    auto owned = std::make_unique<CancellableWorkflow>();
    CancellableWorkflow *workflow = owned.get();
    RecordingDialog dialog(std::move(owned), FlashOperation::kWrite, "rom.bin");
    QTimer::singleShot(0, &dialog,
                       [&dialog, workflow]
                       {
                           ASSERT_TRUE(workflow->active_attempt != nullptr);
                           ASSERT_TRUE(workflow->active_attempt->waitUntilStarted());
                           dialog.close();
                       });
    const FlashDialogResult result = dialog.run();
    ASSERT_EQ(result.outcome, FlashWorkflowOutcome::kCancelled);
    ASSERT_EQ(dialog.prompts, (QList{FlashPromptKind::kBegin, FlashPromptKind::kRemoveProgrammingVoltage}));
    ASSERT_EQ(workflow->attempt_results, QList{ErrorKind::kCancelled});
    ASSERT_TRUE(!dialog.success_shown);

    // The worker emitted finished before closeEvent joined it; that queued
    // delivery must not submit the attempt a second time.
    QCoreApplication::sendPostedEvents();
    QCoreApplication::processEvents();
    ASSERT_EQ(workflow->attempt_results, QList{ErrorKind::kCancelled});
    ASSERT_EQ(dialog.prompts, (QList{FlashPromptKind::kBegin, FlashPromptKind::kRemoveProgrammingVoltage}));
    ASSERT_TRUE(!dialog.success_shown);
}

TEST(FlashDialogTest, runsASecondAttemptAfterAPromptBetweenAttempts)
{
    auto owned = std::make_unique<TwoAttemptWorkflow>();
    TwoAttemptWorkflow *workflow = owned.get();
    RecordingDialog dialog(std::move(owned), FlashOperation::kWrite, "rom.bin");
    const FlashDialogResult result = dialog.run();
    ASSERT_EQ(result.outcome, FlashWorkflowOutcome::kSucceeded);
    ASSERT_EQ(dialog.prompts, (QList{FlashPromptKind::kBegin, FlashPromptKind::kRemoveMod1}));
    ASSERT_EQ(workflow->attempts, (QList{true, true}));
    ASSERT_TRUE(dialog.success_shown);
}

TEST(FlashDialogTest, programmingVoltageNoticeKeepsTheSixC3AdviceByDefault)
{
    const auto notice = FlashDialog::programmingVoltageNotice(
        {FlashPromptKind::kRemoveProgrammingVoltage, {{"outcome", "failed"}, {"external_vpp", "yes"}}});
    ASSERT_EQ(notice.title, QString("Programming voltage"));
    ASSERT_TRUE(notice.text.contains("Remove VPP voltage"));
    ASSERT_TRUE(notice.text.contains("do not power it off"));
}

TEST(FlashDialogTest, programmingVoltageNoticeWithoutPowerOffAdviceOnFailure)
{
    const auto notice = FlashDialog::programmingVoltageNotice(
        {FlashPromptKind::kRemoveProgrammingVoltage,
         {{"outcome", "failed"}, {"external_vpp", "yes"}, {"power_off_advice", "no"}}});
    ASSERT_TRUE(notice.text.contains("Remove VPP voltage"));
    ASSERT_TRUE(!notice.text.contains("do not power it off"));
    ASSERT_TRUE(notice.text.contains("try again"));
}

TEST(FlashDialogTest, programmingVoltageNoticeWithoutPowerOffAdviceOnSuccess)
{
    const auto notice = FlashDialog::programmingVoltageNotice(
        {FlashPromptKind::kRemoveProgrammingVoltage,
         {{"outcome", "succeeded"}, {"external_vpp", "yes"}, {"power_off_advice", "no"}}});
    ASSERT_TRUE(notice.text.contains("Remove VPP voltage"));
    ASSERT_TRUE(notice.text.contains("request SSM Init"));
    ASSERT_TRUE(!notice.text.contains("did not complete"));
}

} // namespace
} // namespace fastecu::flash

namespace
{
const auto *const kApplicationEnvironment =
    ::testing::AddGlobalTestEnvironment(new fastecu::testing::WidgetsApplicationEnvironment({}, /*use_96_dpi=*/true));
}
