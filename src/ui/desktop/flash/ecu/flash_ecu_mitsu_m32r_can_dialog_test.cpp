// Dialog orchestration tests for FlashEcuMitsuM32rCan (step 5 tail, wave 0).
// Same harness shape as the EEPROM dialog tests in
// src/ui/desktop/flash/eeprom: the dialog's real run() orchestration is
// exercised through its protected test seams (confirm()/showFailureDialog())
// so no modal QMessageBox is ever shown and no hardware is touched.
//
// The gate/preflight cases stop before a FlashWorker is ever created -- either
// the operator declines a gate, or the real buildPlan() rejects the
// configuration. The worker cases replace only makeWorker(), so the real
// buildPlan() still runs and the real onWorkerFinished() orchestration is
// what is under test; the executor and clock are scripted doubles. Either
// way a null SerialPortActions is never dereferenced.
//
// Three groups of cases go further than that seam:
//
//  - The surface cases (theFailureDialogExplainsTheErrorKind and friends)
//    drive the REAL confirm()/showSuccessDialog()/showFailureDialog(), which
//    do open a modal QMessageBox; answerModal() below answers it from inside
//    its own nested event loop. What the operator is told is behaviour, and
//    the overrides above are precisely what hides it.
//  - theRealWorkerWiringReportsAMissingAdapter keeps the real makeWorker(),
//    exercising the production executor/transport/clock wiring against a
//    null SerialPortActions -- which the adapter reports as Disconnected
//    before any hardware call.
//  - closingTheDialogMidRunStopsTheWorker runs a worker that blocks until it
//    is asked to stop, and closes the window while it is in flight.
#include <QAbstractButton>
#include <QApplication>
#include <QMessageBox>
#include <QTimer>
#include <QtTest>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h"

using fastecu::FakeClock;
using fastecu::LogLevel;
using fastecu::flash::FlashExecutionResult;
using fastecu::flash::FlashOperation;
using fastecu::flash::FlashPlan;
using fastecu::flash::FlashWorker;

namespace
{

// Minimal IFlashTransport double: ScriptedExecutor below never calls a
// transport method, so this only needs to exist and answer request_unblock().
class NullTransport : public fastecu::flash::IFlashTransport
{
  public:
    void request_unblock() noexcept override
    {
    }
};

// Scripted IFlashExecutor: performs no I/O, reports the scripted events
// through the injected sink (the same seam the real executor logs and
// reports progress through), and returns the configured Result.
class ScriptedExecutor : public fastecu::flash::IFlashExecutor
{
  public:
    fastecu::Result<FlashExecutionResult> nextResult =
        fastecu::fail(fastecu::ErrorKind::BadResponse, "scripted");
    std::vector<std::pair<LogLevel, std::string>> logs;
    std::vector<std::pair<int, int>> progressReports;

    fastecu::Result<FlashExecutionResult> execute(const FlashPlan&,
                                                  fastecu::flash::IFlashTransport&,
                                                  fastecu::IClock&,
                                                  const fastecu::ICancellationToken&,
                                                  fastecu::IEventSink& events) override
    {
        for (const auto& [level, message] : logs)
        {
            events.log(level, message);
        }
        for (const auto& [done, total] : progressReports)
        {
            events.progress(done, total);
        }
        return nextResult;
    }
};

// Runs until the cancellation token is tripped, then reports what it saw.
// Bounded rather than unconditional: a dialog that stopped asking the worker
// to stop must fail this test, not hang it.
class BlockingExecutor : public fastecu::flash::IFlashExecutor
{
  public:
    explicit BlockingExecutor(std::shared_ptr<std::atomic<bool>> sawCancellation)
        : sawCancellation_(std::move(sawCancellation))
    {
    }

    fastecu::Result<FlashExecutionResult> execute(const FlashPlan&,
                                                  fastecu::flash::IFlashTransport&,
                                                  fastecu::IClock&,
                                                  const fastecu::ICancellationToken& cancellation,
                                                  fastecu::IEventSink&) override
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!cancellation.cancelled() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        *sawCancellation_ = cancellation.cancelled();
        return fastecu::fail(fastecu::ErrorKind::Cancelled, "stopped");
    }

  private:
    std::shared_ptr<std::atomic<bool>> sawCancellation_;
};

// What the one modal box `answerModal` drove said.
struct ModalCapture
{
    bool seen = false;
    QString text;
    QMessageBox::Icon icon = QMessageBox::NoIcon;
    QMessageBox::StandardButtons buttons = QMessageBox::NoButton;
};

// Runs `action` -- which opens one real modal QMessageBox -- and answers it
// by clicking `button`, recording what the box said on the way past.
//
// QMessageBox::warning()/::information() spin their own nested event loop, so
// a test can only reach the dialog's REAL (unoverridden) modal surfaces from
// inside that loop; the zero-interval timer below is that hook. The tick
// budget keeps a surface that never opens a box from hanging the suite.
ModalCapture answerModal(const std::function<void()>& action,
                         QMessageBox::StandardButton button)
{
    ModalCapture capture;
    QTimer timer;
    timer.setInterval(0);
    int ticks = 0;
    QObject::connect(&timer, &QTimer::timeout, &timer,
                     [&]
                     {
                         QMessageBox *box = nullptr;
                         const QWidgetList widgets = QApplication::topLevelWidgets();
                         for (QWidget *widget : widgets)
                         {
                             auto *candidate = qobject_cast<QMessageBox *>(widget);
                             if (candidate != nullptr && candidate->isVisible())
                             {
                                 box = candidate;
                             }
                         }
                         if (box == nullptr)
                         {
                             if (++ticks > 10000)
                             {
                                 timer.stop();
                                 QApplication::closeAllWindows();
                             }
                             return;
                         }
                         timer.stop();
                         capture.seen = true;
                         capture.text = box->text();
                         capture.icon = box->icon();
                         capture.buttons = box->standardButtons();
                         if (QAbstractButton *target = box->button(button); target != nullptr)
                         {
                             target->click();
                         }
                         else
                         {
                             box->close();
                         }
                     });
    timer.start();
    action();
    timer.stop();
    return capture;
}

} // namespace

// Records the dialog's modal surfaces instead of showing them and answers
// each prompt from a scripted queue. Split out from the worker-scripting
// subclass below so the "real production wiring" case can keep the real
// makeWorker() while still never opening a modal.
class SurfaceRecordingDialog : public FlashEcuMitsuM32rCan
{
  public:
    using FlashEcuMitsuM32rCan::buildPlan;
    using FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan;

    QStringList confirmTitles;
    QStringList confirmTexts;
    QList<int> confirmAnswers;
    QList<fastecu::ErrorKind> failureKinds;
    QStringList failureDetails;
    int successDialogCount = 0;

  protected:
    int confirm(const QString& title, const QString& text, int buttons,
                int defaultButton) override
    {
        Q_UNUSED(buttons)
        confirmTitles << title;
        confirmTexts << text;
        return confirmAnswers.isEmpty() ? defaultButton : confirmAnswers.takeFirst();
    }

    void showSuccessDialog() override
    {
        ++successDialogCount;
    }

    void showFailureDialog(fastecu::ErrorKind kind, const QString& detail) override
    {
        failureKinds << kind;
        failureDetails << detail;
    }
};

static_assert(!std::is_constructible_v<FlashEcuMitsuM32rCan, SerialPortActions *,
                                       FileActions::EcuCalDefStructure *, const QString&,
                                       QWidget *, bool>);

// Adds the scripted worker: the dialog's real FlashWorker orchestration runs
// against a scripted executor and a deterministic clock.
class TestableFlashEcuMitsuM32rCan : public SurfaceRecordingDialog
{
  public:
    using SurfaceRecordingDialog::SurfaceRecordingDialog;

    // Scripted result of the single attempt this dialog ever makes.
    fastecu::Result<FlashExecutionResult> executorResult =
        fastecu::fail(fastecu::ErrorKind::BadResponse, "scripted bad response");
    // Events the scripted executor reports before returning that result.
    std::vector<std::pair<LogLevel, std::string>> logs;
    std::vector<std::pair<int, int>> progressReports;
    bool makeWorkerCalled = false;

  protected:
    std::unique_ptr<FlashWorker> makeWorker(FlashPlan plan) override
    {
        makeWorkerCalled = true;
        auto executor = std::make_unique<ScriptedExecutor>();
        executor->nextResult = executorResult;
        executor->logs = logs;
        executor->progressReports = progressReports;
        return std::make_unique<FlashWorker>(std::move(plan), std::move(executor),
                                             std::make_unique<NullTransport>(),
                                             std::make_unique<FakeClock>());
    }
};

// Keeps the dialog's real makeWorker() -- and with it the real executor,
// transport adapter and clock -- while still recording its modal surfaces.
using RealWorkerDialog = SurfaceRecordingDialog;

// Keeps every real modal surface, exposed so a test can drive one directly.
class RealSurfaceDialog : public FlashEcuMitsuM32rCan
{
  public:
    using FlashEcuMitsuM32rCan::confirm;
    using FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan;
    using FlashEcuMitsuM32rCan::showFailureDialog;
    using FlashEcuMitsuM32rCan::showSuccessDialog;
};

// Blocks in the executor until the dialog asks the worker to stop.
class BlockingWorkerDialog : public SurfaceRecordingDialog
{
  public:
    using SurfaceRecordingDialog::SurfaceRecordingDialog;

    std::shared_ptr<std::atomic<bool>> sawCancellation =
        std::make_shared<std::atomic<bool>>(false);

  protected:
    std::unique_ptr<FlashWorker> makeWorker(FlashPlan plan) override
    {
        return std::make_unique<FlashWorker>(std::move(plan),
                                             std::make_unique<BlockingExecutor>(sawCancellation),
                                             std::make_unique<NullTransport>(),
                                             std::make_unique<FakeClock>());
    }
};

class TestFlashEcuMitsuM32rCanDialog : public QObject
{
    Q_OBJECT
  private slots:
    void protocolSelectsCapacityAndAuthorization()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can_vendor_ext_512kb";
        SurfaceRecordingDialog dialog(nullptr, &ecuCalDef, "read", nullptr);

        auto plan = dialog.buildPlan();

        QVERIFY(plan.has_value());
        const auto& family =
            std::get<fastecu::flash::MitsuColtM32rCanPlan>(plan->family_plan());
        QCOMPARE(family.use_vendor_challenge, true);
        QCOMPARE(family.rom_size, std::uint32_t{0x80000});
    }

    void readDeclinedAtIgnitionPromptStartsNoWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Cancel;

        dialog.run();

        QCOMPARE(dialog.confirmTitles.size(), 1);
        QCOMPARE(dialog.confirmTitles.at(0), QString("Connecting to ECU"));
        QVERIFY(dialog.failureKinds.isEmpty());
    }

    void unknownMcuIsRejectedBeforeAnyWorkerStarts()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "NOT_A_REAL_MCU";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::InvalidConfig);
    }

    void testWriteIsRejectedAsUnsupported()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "test_write", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::Unsupported);
    }

    void writeDeclinedAtEraseGateStartsNoWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x60000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr);
        // Ignition OK, erase trigger declined: stop before any worker is built.
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Cancel;

        dialog.run();

        QCOMPARE(dialog.confirmTitles,
                 QStringList({"Connecting to ECU", "Erase trigger"}));
        QVERIFY(dialog.confirmTexts.at(1).contains("384 KiB"));
        QVERIFY(dialog.failureKinds.isEmpty());
        QVERIFY(!dialog.makeWorkerCalled);
    }

    void writeAsksTheTopRegionGateAfterTheEraseGate()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can_512kb";
        ecuCalDef.FullRomData = QByteArray(0x80000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Yes << QMessageBox::Cancel;

        dialog.run();

        QCOMPARE(dialog.confirmTitles,
                 QStringList({"Connecting to ECU", "Erase trigger",
                              "Top 128KB bootstrap"}));
        QVERIFY(dialog.confirmTexts.at(1).contains("512 KiB"));
    }

    void romTooSmallIsRejectedAsInvalidConfig()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x60000 - 1, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Yes << QMessageBox::Yes;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::InvalidConfig);
        QVERIFY(dialog.confirmTitles.isEmpty());
        QVERIFY(!dialog.makeWorkerCalled);
    }

    void romTooLargeIsRejectedBeforeAnyConfirmationOrWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x60000 + 1, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr);

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::InvalidConfig);
        QVERIFY(dialog.confirmTitles.isEmpty());
        QVERIFY(!dialog.makeWorkerCalled);
    }

    void writeWarningsExplainTheProtectedPrefixWritableRangeAndCancellationRisk()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x60000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Cancel;

        dialog.run();

        const QString warnings = dialog.confirmTexts.join("\n");
        QVERIFY(warnings.contains("first 32 KiB"));
        QVERIFY(warnings.contains("0x0000-0x8000"));
        QVERIFY(warnings.contains("0x8000-0x60000"));
        QVERIFY(warnings.contains("bootloader will remain unchanged"));
        QVERIFY(warnings.contains("Cancellation after erase"));
        QVERIFY(warnings.contains("requiring recovery"));
    }

    // The positive path: every gate granted and a valid configuration must
    // actually reach a worker. Without this, a regression that returned early
    // after the last gate would satisfy every case above.
    void writeWithEveryGateGrantedReachesTheWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x60000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Yes;
        dialog.executorResult = FlashExecutionResult{.operation = FlashOperation::Write,
                                                     .read_bytes = std::nullopt};

        dialog.run();

        QVERIFY(dialog.makeWorkerCalled);
        QCOMPARE(dialog.confirmTitles,
                 QStringList({"Connecting to ECU", "Erase trigger"}));
        QCOMPARE(dialog.successDialogCount, 1);
        QVERIFY(dialog.failureKinds.isEmpty());
        // A write carries no read bytes, so the ROM the user supplied is left
        // exactly as it was.
        QCOMPARE(ecuCalDef.FullRomData, QByteArray(0x60000, '\0'));
    }

    void successfulReadCopiesTheBytesIntoFullRomData()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;
        const std::vector<std::uint8_t> readBytes{0xDE, 0xAD, 0xBE, 0xEF};
        dialog.executorResult =
            FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = readBytes};

        dialog.run();

        QVERIFY(dialog.makeWorkerCalled);
        QCOMPARE(dialog.successDialogCount, 1);
        QVERIFY(dialog.failureKinds.isEmpty());
        QCOMPARE(ecuCalDef.FullRomData,
                 QByteArray(reinterpret_cast<const char *>(readBytes.data()),
                            static_cast<qsizetype>(readBytes.size())));
    }

    void workerFailureIsReportedWithItsOwnErrorKind_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("Timeout") << static_cast<int>(fastecu::ErrorKind::Timeout);
        QTest::newRow("Disconnected") << static_cast<int>(fastecu::ErrorKind::Disconnected);
        QTest::newRow("BadResponse") << static_cast<int>(fastecu::ErrorKind::BadResponse);
        QTest::newRow("Internal") << static_cast<int>(fastecu::ErrorKind::Internal);
    }

    void workerFailureIsReportedWithItsOwnErrorKind()
    {
        QFETCH(int, kind);
        const auto errorKind = static_cast<fastecu::ErrorKind>(kind);

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;
        dialog.executorResult = fastecu::fail(errorKind, "scripted");

        dialog.run();

        QVERIFY(dialog.makeWorkerCalled);
        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), errorKind);
        QCOMPARE(dialog.successDialogCount, 0);
        QVERIFY(ecuCalDef.FullRomData.isEmpty());
    }

    // Cancelled is the operator's own stop, not a fault: it closes the dialog
    // silently rather than reporting an error.
    void cancelledWorkerShowsNoFailureDialog()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;
        dialog.executorResult = fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted stop");

        dialog.run();

        QVERIFY(dialog.makeWorkerCalled);
        QVERIFY(dialog.failureKinds.isEmpty());
        QCOMPARE(dialog.successDialogCount, 0);
    }

    // The executor's log levels are the only thing that decides which log
    // signal a line leaves on, and the application log colours/filters by
    // signal: an error surfacing as LOG_I is a flash failure the operator
    // never sees. Each level carries its own text so a crossed arm fails
    // here rather than looking like a passing count.
    void logEventsAreRoutedToTheLogSignalForTheirLevel()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;
        dialog.logs = {{LogLevel::Error, "an error"},
                       {LogLevel::Warning, "a warning"},
                       {LogLevel::Info, "an info"},
                       {LogLevel::Debug, "a debug"}};
        dialog.executorResult =
            FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = std::nullopt};

        QSignalSpy errors(&dialog, &FlashEcuMitsuM32rCan::LOG_E);
        QSignalSpy warnings(&dialog, &FlashEcuMitsuM32rCan::LOG_W);
        QSignalSpy infos(&dialog, &FlashEcuMitsuM32rCan::LOG_I);
        QSignalSpy debugs(&dialog, &FlashEcuMitsuM32rCan::LOG_D);

        dialog.run();

        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.at(0).at(0).toString(), QString("an error"));
        // Legacy call shape: every line is timestamped and linefeed-terminated.
        QCOMPARE(errors.at(0).at(1).toBool(), true);
        QCOMPARE(errors.at(0).at(2).toBool(), true);
        QCOMPARE(warnings.size(), 1);
        QCOMPARE(warnings.at(0).at(0).toString(), QString("a warning"));
        QCOMPARE(infos.size(), 1);
        QCOMPARE(infos.at(0).at(0).toString(), QString("an info"));
        QCOMPARE(debugs.size(), 1);
        QCOMPARE(debugs.at(0).at(0).toString(), QString("a debug"));
    }

    // The executor reports bytes done / bytes total; the progress bar wants a
    // percentage. Reporting bytes straight through would peg the bar at 100%
    // from the first chunk of a 384KB read.
    void progressIsConvertedToAPercentageOfTheTotal()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;
        dialog.progressReports = {{0, 400}, {100, 400}, {399, 400}, {400, 400}};
        dialog.executorResult =
            FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = std::nullopt};

        QSignalSpy percentages(&dialog,
                               QOverload<int>::of(&FlashEcuMitsuM32rCan::external_logger));

        dialog.run();

        // 0/400 repeats the value run() already set, and is suppressed;
        // 399/400 truncates to 99 rather than rounding up to a premature 100.
        QList<int> values;
        for (const QList<QVariant>& call : percentages)
        {
            values << call.at(0).toInt();
        }
        QCOMPARE(values, QList<int>({25, 99, 100}));
    }

    // A total of zero is what an executor reports when it cannot know the
    // size up front. The percentage math has to survive it: dividing would
    // fault the process mid-flash.
    void aZeroTotalProgressReportIsPassedThroughUndivided()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;
        dialog.progressReports = {{7, 0}};
        dialog.executorResult =
            FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = std::nullopt};

        QSignalSpy percentages(&dialog,
                               QOverload<int>::of(&FlashEcuMitsuM32rCan::external_logger));

        dialog.run();

        QCOMPARE(percentages.size(), 1);
        QCOMPARE(percentages.at(0).at(0).toInt(), 7);
    }

    // Closing the window is the operator's abort. It has to reach the worker:
    // an ECU left mid-erase because the dialog went away without stopping the
    // run is exactly the failure mode the cancellation contract exists for.
    void closingTheDialogMidRunStopsTheWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        BlockingWorkerDialog dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;
        auto sawCancellation = dialog.sawCancellation;

        // Fires from inside run()'s own event loop, with the worker started.
        QTimer::singleShot(0, &dialog, [&dialog]
                           { dialog.close(); });

        dialog.run();

        QTRY_VERIFY(sawCancellation->load());
        QVERIFY(dialog.failureKinds.isEmpty());
        QCOMPARE(dialog.successDialogCount, 0);
    }

    // The production wiring, end to end: the dialog's own makeWorker(), the
    // real MitsuColtM32rCanExecutor, the real DesktopCanFlashTransport and
    // the real QtClock. With no SerialPortActions behind it the adapter
    // reports Disconnected from configure(), before a single byte could
    // reach an ECU -- so this exercises the real chain without hardware.
    void theRealWorkerWiringReportsAMissingAdapter()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        RealWorkerDialog dialog(nullptr, &ecuCalDef, "read", nullptr);
        dialog.confirmAnswers << QMessageBox::Ok;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::Disconnected);
        QCOMPARE(dialog.successDialogCount, 0);
        QVERIFY(ecuCalDef.FullRomData.isEmpty());
    }

    void theFailureDialogExplainsTheErrorKind_data()
    {
        QTest::addColumn<int>("kind");
        QTest::addColumn<QString>("text");
        QTest::newRow("InvalidConfig")
            << static_cast<int>(fastecu::ErrorKind::InvalidConfig)
            << QString("ECU flash configuration is invalid or unsupported for this operation. "
                       "Check the ROM definition and selected protocol, then try again.");
        QTest::newRow("Unsupported")
            << static_cast<int>(fastecu::ErrorKind::Unsupported)
            << QString("ECU flash configuration is invalid or unsupported for this operation. "
                       "Check the ROM definition and selected protocol, then try again.");
        QTest::newRow("Disconnected")
            << static_cast<int>(fastecu::ErrorKind::Disconnected)
            << QString("Lost connection to the adapter or ECU. Check the cable/adapter "
                       "connection, press OK to exit and try again.");
        QTest::newRow("Timeout") << static_cast<int>(fastecu::ErrorKind::Timeout)
                                 << QString("ECU did not respond in time, press OK to exit and "
                                            "try again.");
        QTest::newRow("BadResponse")
            << static_cast<int>(fastecu::ErrorKind::BadResponse)
            << QString("ECU returned an unexpected or rejected response, press OK to exit and "
                       "try again.");
        QTest::newRow("Internal") << static_cast<int>(fastecu::ErrorKind::Internal)
                                  << QString("ECU operation failed, press OK to exit and try "
                                             "again");
    }

    // The real, unoverridden failure surface. What the operator is told is
    // the whole point of the ErrorKind split -- "check the cable" and "check
    // the ROM definition" send someone down completely different paths.
    void theFailureDialogExplainsTheErrorKind()
    {
        QFETCH(int, kind);
        QFETCH(QString, text);
        const auto errorKind = static_cast<fastecu::ErrorKind>(kind);

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        RealSurfaceDialog dialog(nullptr, &ecuCalDef, "read", nullptr);
        QSignalSpy errors(&dialog, &FlashEcuMitsuM32rCan::LOG_E);

        const ModalCapture capture = answerModal(
            [&]
            { dialog.showFailureDialog(errorKind, "scripted detail"); }, QMessageBox::Ok);

        QVERIFY(capture.seen);
        QCOMPARE(capture.text, text);
        QCOMPARE(capture.icon, QMessageBox::Warning);
        // The log line names the kind and keeps the raw detail, which is the
        // only place the executor's own message survives.
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.at(0).at(0).toString(),
                 QString("ECU operation failed (%1): scripted detail")
                     .arg(QString::fromUtf8(fastecu::to_string(errorKind))));
    }

    // Cancelled is the operator's own stop: onWorkerFinished() short-circuits
    // it, and the surface itself must stay silent even if it is reached.
    void theFailureDialogShowsNoBoxForACancelledRun()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        RealSurfaceDialog dialog(nullptr, &ecuCalDef, "read", nullptr);
        QSignalSpy errors(&dialog, &FlashEcuMitsuM32rCan::LOG_E);

        const ModalCapture capture = answerModal(
            [&]
            { dialog.showFailureDialog(fastecu::ErrorKind::Cancelled, "stopped"); },
            QMessageBox::Ok);

        QVERIFY(!capture.seen);
        QCOMPARE(errors.size(), 1);
    }

    void theSuccessDialogAnnouncesTheOperationFinished()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        RealSurfaceDialog dialog(nullptr, &ecuCalDef, "read", nullptr);

        const ModalCapture capture =
            answerModal([&]
                        { dialog.showSuccessDialog(); }, QMessageBox::Ok);

        QVERIFY(capture.seen);
        // Verbatim legacy text, the original's spelling of "succesful" included.
        QCOMPARE(capture.text, QString("ECU operation was succesful, press OK to exit"));
        QCOMPARE(capture.icon, QMessageBox::Information);
    }

    // The real confirm(): every gate in run() is a comparison against what
    // this returns, so it has to be the operator's actual click and not the
    // default it was offered.
    void confirmReturnsTheButtonTheOperatorClicked()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        RealSurfaceDialog dialog(nullptr, &ecuCalDef, "read", nullptr);

        int answer = QMessageBox::NoButton;
        const ModalCapture capture = answerModal(
            [&]
            {
                answer = dialog.confirm("Connecting to ECU", "Turn ignition ON",
                                        QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok);
            },
            QMessageBox::Cancel);

        QVERIFY(capture.seen);
        QCOMPARE(capture.text, QString("Turn ignition ON"));
        QCOMPARE(capture.buttons, QMessageBox::Ok | QMessageBox::Cancel);
        QCOMPARE(answer, static_cast<int>(QMessageBox::Cancel));
    }
};

QTEST_MAIN(TestFlashEcuMitsuM32rCanDialog)
#include "flash_ecu_mitsu_m32r_can_dialog_test.moc"
