// Dialog orchestration tests for EepromEcuSubaruDensoSH705xKline. Exercises
// the dialog's one-attempt-per-mode orchestration against scripted
// IFlashExecutor/IFlashTransport doubles
// injected through the dialog's protected test seams (buildPlan()/
// makeWorker()/confirm()/showFailureDialog()), passing the dialog's required
// ConfigPaths value while overriding virtuals rather than injecting fakes
// through a production constructor.
#include "src/ui/desktop/flash/eeprom/eeprom_ecu_subaru_denso_sh705x_kline.h"

#include <QApplication>
#include <QByteArray>
#include <QMessageBox>
#include <QTest>

#include <cstddef>
#include <memory>
#include <vector>

#include "src/backend/flash/eeprom/denso_sh705x_eeprom_common.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "test_eeprom_ecu_subaru_denso_sh705x_kline_dialog.h"

using fastecu::ErrorKind;
using fastecu::FakeClock;
using fastecu::flash::DensoSecurityVariant;
using fastecu::flash::DensoSh705xEepromInput;
using fastecu::flash::DensoSh705xEepromKlinePlan;
using fastecu::flash::EepromReadMode;
using fastecu::flash::FlashExecutionResult;
using fastecu::flash::FlashFamily;
using fastecu::flash::FlashOperation;
using fastecu::flash::FlashPlan;
using fastecu::flash::FlashWorker;
using fastecu::flash::KernelImage;
using fastecu::flash::MemoryRegion;

namespace
{

// Minimal IFlashTransport double: ScriptedExecutor below never actually
// calls a transport method, so this only needs to exist and answer
// request_unblock().
class NullTransport : public fastecu::flash::IFlashTransport
{
  public:
    void request_unblock() noexcept override
    {
    }
};

// Scripted IFlashExecutor: always returns the configured Result, and
// records which mode each execute() call was for (extracted from the
// plan's family_plan() variant) in call order.
class ScriptedExecutor : public fastecu::flash::IFlashExecutor
{
  public:
    fastecu::Result<FlashExecutionResult> nextResult = fastecu::fail(ErrorKind::BadResponse, "scripted");
    std::vector<EepromReadMode> *seenModes = nullptr;

    fastecu::Result<FlashExecutionResult> execute(const FlashPlan& plan,
                                                  fastecu::flash::IFlashTransport&, fastecu::IClock&,
                                                  const fastecu::ICancellationToken&,
                                                  fastecu::IEventSink&) override
    {
        if (seenModes)
        {
            seenModes->push_back(std::get<DensoSh705xEepromKlinePlan>(plan.family_plan()).mode);
        }
        return nextResult;
    }
};

// Every field here matches an SH7055 K-Line EEPROM read plan that
// build_denso_sh705x_eeprom_plan/validate_and_build accept outright for
// every one of Mode2/Mode3/Mode4 -- see
// src/backend/flash/eeprom/denso_sh705x_eeprom_common.cpp's
// resolve_mcu_bounds("SH7055") for the eeprom/kernel-RAM bounds this must
// satisfy.
fastecu::Result<FlashPlan> makePlan(EepromReadMode mode)
{
    return fastecu::flash::build_denso_sh705x_eeprom_plan(DensoSh705xEepromInput{
        .operation = FlashOperation::Read,
        .family = FlashFamily::DensoSh705xEepromKline,
        .target_id = "sub_ecu_eeprom_denso_sh7055_kline",
        .mcu_name = "SH7055",
        .flash_method = "sub_ecu_eeprom_denso_sh7055_kline",
        .kernel = KernelImage{.id = "k", .load_address = 0xFFFF6004, .bytes = {0x01}},
        .mode = mode,
        .security = DensoSecurityVariant::Stock,
        .eeprom_region = MemoryRegion{.start = 0, .length = 0x100},
    });
}

} // namespace

// Test seam subclass: overrides buildPlan()/makeWorker()/confirm()/
// showFailureDialog() so the dialog's real orchestration logic (runAttempt/
// onWorkerFinished in the .cpp) runs against scripted, deterministic
// results instead of hardware access or a real modal QMessageBox.
class TestableKlineDialog : public EepromEcuSubaruDensoSH705xKline
{
  public:
    using EepromEcuSubaruDensoSH705xKline::EepromEcuSubaruDensoSH705xKline;

    // Scripted confirm() answers, consumed in order as calls happen; the
    // last entry repeats once exhausted so a test that only cares about the
    // first N confirm() calls doesn't need to enumerate every subsequent one.
    std::vector<int> confirmAnswers{QMessageBox::Ok};
    std::size_t confirmCallIndex = 0;
    int confirmCallCount = 0;

    // Every FlashWorker this dialog builds gets a fresh ScriptedExecutor
    // returning this same scripted result and sharing this same seenModes
    // vector, so a test can script "every attempt fails/succeeds the same
    // way" and still observe the full mode sequence across the whole run.
    fastecu::Result<FlashExecutionResult> executorResult =
        fastecu::fail(ErrorKind::BadResponse, "scripted bad response");
    std::vector<EepromReadMode> seenModes;
    std::vector<EepromReadMode> planRequestedModes;
    bool buildPlanFails = false;
    bool makeWorkerCalled = false;

    int showFailureDialogCallCount = 0;
    fastecu::ErrorKind lastFailureKind = fastecu::ErrorKind::Internal;

  protected:
    fastecu::Result<FlashPlan> buildPlan(EepromReadMode mode) override
    {
        planRequestedModes.push_back(mode);
        if (buildPlanFails)
        {
            return fastecu::fail(ErrorKind::InvalidConfig, "scripted plan failure");
        }
        return makePlan(mode);
    }

    std::unique_ptr<FlashWorker> makeWorker(FlashPlan plan) override
    {
        makeWorkerCalled = true;
        auto executor = std::make_unique<ScriptedExecutor>();
        executor->nextResult = executorResult;
        executor->seenModes = &seenModes;
        return std::make_unique<FlashWorker>(std::move(plan), std::move(executor),
                                             std::make_unique<NullTransport>(),
                                             std::make_unique<FakeClock>());
    }

    int confirm(const QString&, const QString&, int, int) override
    {
        ++confirmCallCount;
        const std::size_t index = std::min(confirmCallIndex, confirmAnswers.size() - 1);
        const int answer = confirmAnswers.at(index);
        if (confirmCallIndex + 1 < confirmAnswers.size())
        {
            ++confirmCallIndex;
        }
        return answer;
    }

    void showFailureDialog(fastecu::ErrorKind kind, const QString&) override
    {
        ++showFailureDialogCallCount;
        lastFailureKind = kind;
    }
};

class TestEepromKlineDialog : public QObject
{
    Q_OBJECT

  private slots:

    void modeOrchestrationIsExactlyTwoThreeFourNeverRepeatsOrSkips()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        TestableKlineDialog dialog(nullptr, &ecuCalDef, fastecu::config::ConfigPaths{}, "read");
        dialog.executorResult = fastecu::fail(ErrorKind::BadResponse, "scripted bad response");
        dialog.confirmAnswers = {QMessageBox::Ok}; // pre-flight only

        dialog.run();

        QCOMPARE(dialog.seenModes.size(), std::size_t(3));
        QCOMPARE(static_cast<int>(dialog.seenModes.at(0)), static_cast<int>(EepromReadMode::Mode2));
        QCOMPARE(static_cast<int>(dialog.seenModes.at(1)), static_cast<int>(EepromReadMode::Mode3));
        QCOMPARE(static_cast<int>(dialog.seenModes.at(2)), static_cast<int>(EepromReadMode::Mode4));
        // Exactly one failure dialog, after the last (mode 4) attempt --
        // never a dialog per attempt, never a fourth attempt.
        QCOMPARE(dialog.showFailureDialogCallCount, 1);
    }

    void initialDeclineDoesNotShowAGenericErrorDialog()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        TestableKlineDialog dialog(nullptr, &ecuCalDef, fastecu::config::ConfigPaths{}, "read");
        dialog.confirmAnswers = {QMessageBox::Cancel};

        dialog.run();

        QVERIFY(!dialog.makeWorkerCalled);
        QCOMPARE(dialog.seenModes.size(), std::size_t(0));
        QCOMPARE(dialog.showFailureDialogCallCount, 0);
    }

    void firstAttemptPlanErrorReturnsWithoutCreatingWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        TestableKlineDialog dialog(nullptr, &ecuCalDef, fastecu::config::ConfigPaths{}, "read");
        dialog.buildPlanFails = true;
        dialog.confirmAnswers = {QMessageBox::Ok};

        dialog.run();

        QVERIFY(!dialog.makeWorkerCalled);
        QCOMPARE(dialog.planRequestedModes.size(), std::size_t(1));
        QCOMPARE(static_cast<int>(dialog.planRequestedModes.front()),
                 static_cast<int>(EepromReadMode::Mode2));
        QCOMPARE(dialog.seenModes.size(), std::size_t(0));
        QCOMPARE(dialog.showFailureDialogCallCount, 1);
        QCOMPARE(static_cast<int>(dialog.lastFailureKind),
                 static_cast<int>(ErrorKind::InvalidConfig));
    }

    void acceptedBytesAloneUpdateFullRomData()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        TestableKlineDialog dialog(nullptr, &ecuCalDef, fastecu::config::ConfigPaths{}, "read");
        const std::vector<std::uint8_t> readBytes{0xDE, 0xAD, 0xBE, 0xEF};
        dialog.executorResult =
            FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = readBytes};
        // Pre-flight Ok, then Save on the mode-2 preview.
        dialog.confirmAnswers = {QMessageBox::Ok, QMessageBox::Save};

        dialog.run();

        // Save exits immediately on the first successful attempt -- no
        // discarded intermediate attempt ever ran, so there is nothing else
        // that could have touched FullRomData.
        QCOMPARE(dialog.seenModes.size(), std::size_t(1));
        QCOMPARE(ecuCalDef.FullRomData,
                 QByteArray(reinterpret_cast<const char *>(readBytes.data()),
                            static_cast<qsizetype>(readBytes.size())));
    }

    void structuredFailureProducesExactlyOneFamilySpecificDialogPerErrorKind_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("Timeout") << static_cast<int>(ErrorKind::Timeout);
        QTest::newRow("Disconnected") << static_cast<int>(ErrorKind::Disconnected);
        QTest::newRow("BadResponse") << static_cast<int>(ErrorKind::BadResponse);
        QTest::newRow("Internal") << static_cast<int>(ErrorKind::Internal);
    }

    void structuredFailureProducesExactlyOneFamilySpecificDialogPerErrorKind()
    {
        QFETCH(int, kind);
        const auto errorKind = static_cast<ErrorKind>(kind);

        FileActions::EcuCalDefStructure ecuCalDef;
        TestableKlineDialog dialog(nullptr, &ecuCalDef, fastecu::config::ConfigPaths{}, "read");
        dialog.executorResult = fastecu::fail(errorKind, "scripted");
        dialog.confirmAnswers = {QMessageBox::Ok};

        dialog.run();

        // Every attempt (2, 3, 4) fails with the same kind; only the last
        // one produces a dialog -- never a diagnostic-event-only attempt
        // plus a duplicate generic dialog, and never one dialog per attempt.
        QCOMPARE(dialog.seenModes.size(), std::size_t(3));
        QCOMPARE(dialog.showFailureDialogCallCount, 1);
        QCOMPARE(static_cast<int>(dialog.lastFailureKind), static_cast<int>(errorKind));
    }

    // Regression test: this dialog's buildPlan() unconditionally calls the
    // portable build_eeprom_read_plan() use case -- it has no write
    // path at all. MainWindow dispatches this dialog by protocol name only,
    // and cmd_type flows through unfiltered from the generic top-level menu
    // commands, so a user who selects "Write ROM to ECU" (cmd_type "write")
    // or "Test write ROM to ECU" (cmd_type "test_write") on this protocol
    // must never reach a real EEPROM read -- the dialog must reject before
    // buildPlan()/makeWorker() and must never touch FullRomData.
    void nonReadCmdTypeIsRejectedBeforeBuildingAnyPlanOrWorker_data()
    {
        QTest::addColumn<QString>("cmdType");
        QTest::newRow("write") << QString("write");
        QTest::newRow("test_write") << QString("test_write");
    }

    void nonReadCmdTypeIsRejectedBeforeBuildingAnyPlanOrWorker()
    {
        QFETCH(QString, cmdType);

        FileActions::EcuCalDefStructure ecuCalDef;
        TestableKlineDialog dialog(nullptr, &ecuCalDef, fastecu::config::ConfigPaths{}, cmdType);
        // A successful read result, scripted so that if the guard were
        // absent (or bypassed) the attempt would succeed and the follow-up
        // Save confirm() would overwrite FullRomData -- proving the guard,
        // not an unrelated scripted failure, is what stops this.
        const std::vector<std::uint8_t> readBytes{0xDE, 0xAD, 0xBE, 0xEF};
        dialog.executorResult =
            FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = readBytes};
        // Pre-flight Ok, then Save if a preview dialog were ever (wrongly)
        // reached.
        dialog.confirmAnswers = {QMessageBox::Ok, QMessageBox::Save};

        dialog.run();

        QVERIFY(!dialog.makeWorkerCalled);
        QCOMPARE(dialog.planRequestedModes.size(), std::size_t(0));
        QCOMPARE(dialog.seenModes.size(), std::size_t(0));
        QCOMPARE(dialog.showFailureDialogCallCount, 1);
        QCOMPARE(static_cast<int>(dialog.lastFailureKind),
                 static_cast<int>(ErrorKind::Unsupported));
        QVERIFY(ecuCalDef.FullRomData.isEmpty());
    }
};

int run_test_eeprom_ecu_subaru_denso_sh705x_kline_dialog(int argc, char **argv)
{
    // The dialog constructs real widgets (ui->setupUi(this)), which needs a
    // QApplication rather than a plain QCoreApplication to construct.
    QApplication app(argc, argv);
    TestEepromKlineDialog t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_eeprom_ecu_subaru_denso_sh705x_kline_dialog.moc"
