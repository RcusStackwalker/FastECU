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
#include <QApplication>
#include <QMessageBox>
#include <QtTest>

#include <cstdint>
#include <memory>
#include <vector>

#include "src/backend/definitions/file_actions.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h"

using fastecu::FakeClock;
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

// Scripted IFlashExecutor: performs no I/O and returns the configured Result.
class ScriptedExecutor : public fastecu::flash::IFlashExecutor
{
  public:
    fastecu::Result<FlashExecutionResult> nextResult =
        fastecu::fail(fastecu::ErrorKind::BadResponse, "scripted");

    fastecu::Result<FlashExecutionResult> execute(const FlashPlan&,
                                                  fastecu::flash::IFlashTransport&,
                                                  fastecu::IClock&,
                                                  const fastecu::ICancellationToken&,
                                                  fastecu::IEventSink&) override
    {
        return nextResult;
    }
};

} // namespace

// Records confirmations and dialogs instead of showing modals, answers each
// prompt from a scripted queue, and runs the dialog's real FlashWorker
// orchestration against a scripted executor and a deterministic clock.
class TestableFlashEcuMitsuM32rCan : public FlashEcuMitsuM32rCan
{
  public:
    using FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan;

    QStringList confirmTitles;
    QList<int> confirmAnswers;
    QList<fastecu::ErrorKind> failureKinds;
    int successDialogCount = 0;

    // Scripted result of the single attempt this dialog ever makes.
    fastecu::Result<FlashExecutionResult> executorResult =
        fastecu::fail(fastecu::ErrorKind::BadResponse, "scripted bad response");
    bool makeWorkerCalled = false;

  protected:
    std::unique_ptr<FlashWorker> makeWorker(FlashPlan plan) override
    {
        makeWorkerCalled = true;
        auto executor = std::make_unique<ScriptedExecutor>();
        executor->nextResult = executorResult;
        return std::make_unique<FlashWorker>(std::move(plan), std::move(executor),
                                             std::make_unique<NullTransport>(),
                                             std::make_unique<FakeClock>());
    }

    int confirm(const QString& title, const QString& text, int buttons,
                int defaultButton) override
    {
        Q_UNUSED(text)
        Q_UNUSED(buttons)
        confirmTitles << title;
        return confirmAnswers.isEmpty() ? defaultButton : confirmAnswers.takeFirst();
    }

    void showSuccessDialog() override
    {
        ++successDialogCount;
    }

    void showFailureDialog(fastecu::ErrorKind kind, const QString& detail) override
    {
        Q_UNUSED(detail)
        failureKinds << kind;
    }
};

class TestFlashEcuMitsuM32rCanDialog : public QObject
{
    Q_OBJECT
  private slots:
    void readDeclinedAtIgnitionPromptStartsNoWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr, false);
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
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr, false);
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
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "test_write", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::Unsupported);
    }

    void writeCollectsBothGatesBeforeBuildingAPlan()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x80000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr, false);
        // Ignition OK, erase trigger declined: stop before any plan is built.
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Cancel;

        dialog.run();

        QCOMPARE(dialog.confirmTitles.size(), 2);
        QCOMPARE(dialog.confirmTitles.at(1), QString("Erase trigger"));
        QVERIFY(dialog.failureKinds.isEmpty());
    }

    void writeAsksTheTopRegionGateAfterTheEraseGate()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x80000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Yes << QMessageBox::Cancel;

        dialog.run();

        QCOMPARE(dialog.confirmTitles.size(), 3);
        QCOMPARE(dialog.confirmTitles.at(2), QString("Top 128KB bootstrap"));
    }

    void romTooSmallIsRejectedAsInvalidConfig()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x80000 - 1, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Yes << QMessageBox::Yes;

        dialog.run();

        QCOMPARE(dialog.failureKinds.size(), 1);
        QCOMPARE(dialog.failureKinds.at(0), fastecu::ErrorKind::InvalidConfig);
    }

    // The positive path: every gate granted and a valid configuration must
    // actually reach a worker. Without this, a regression that returned early
    // after the last gate would satisfy every case above.
    void writeWithEveryGateGrantedReachesTheWorker()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        ecuCalDef.FullRomData = QByteArray(0x80000, '\0');
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "write", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok << QMessageBox::Yes << QMessageBox::Yes;
        dialog.executorResult = FlashExecutionResult{.operation = FlashOperation::Write,
                                                     .read_bytes = std::nullopt};

        dialog.run();

        QVERIFY(dialog.makeWorkerCalled);
        QCOMPARE(dialog.confirmTitles.size(), 3);
        QCOMPARE(dialog.successDialogCount, 1);
        QVERIFY(dialog.failureKinds.isEmpty());
        // A write carries no read bytes, so the ROM the user supplied is left
        // exactly as it was.
        QCOMPARE(ecuCalDef.FullRomData, QByteArray(0x80000, '\0'));
    }

    void successfulReadCopiesTheBytesIntoFullRomData()
    {
        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FlashMethod = "mitsu_ecu_m32r_can";
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr, false);
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
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr, false);
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
        TestableFlashEcuMitsuM32rCan dialog(nullptr, &ecuCalDef, "read", nullptr, false);
        dialog.confirmAnswers << QMessageBox::Ok;
        dialog.executorResult = fastecu::fail(fastecu::ErrorKind::Cancelled, "scripted stop");

        dialog.run();

        QVERIFY(dialog.makeWorkerCalled);
        QVERIFY(dialog.failureKinds.isEmpty());
        QCOMPARE(dialog.successDialogCount, 0);
    }
};

QTEST_MAIN(TestFlashEcuMitsuM32rCanDialog)
#include "flash_ecu_mitsu_m32r_can_dialog_test.moc"
