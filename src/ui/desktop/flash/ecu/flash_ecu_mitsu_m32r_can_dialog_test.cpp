// Dialog orchestration tests for FlashEcuMitsuM32rCan (step 5 tail, wave 0).
// Same harness shape as the EEPROM dialog tests in
// src/ui/desktop/flash/eeprom: the dialog's real run() orchestration is
// exercised through its protected test seams (confirm()/showFailureDialog())
// so no modal QMessageBox is ever shown and no hardware is touched.
//
// Every case here stops before a FlashWorker is ever created -- either the
// operator declines a gate, or the real buildPlan() rejects the
// configuration -- so a null SerialPortActions is never dereferenced.
#include <QApplication>
#include <QMessageBox>
#include <QtTest>

#include "src/backend/definitions/file_actions.h"
#include "src/ui/desktop/flash/ecu/flash_ecu_mitsu_m32r_can.h"

// Records confirmations and failure dialogs instead of showing modals, and
// answers each prompt from a scripted queue.
class TestableFlashEcuMitsuM32rCan : public FlashEcuMitsuM32rCan
{
  public:
    using FlashEcuMitsuM32rCan::FlashEcuMitsuM32rCan;

    QStringList confirmTitles;
    QList<int> confirmAnswers;
    QList<fastecu::ErrorKind> failureKinds;

  protected:
    int confirm(const QString& title, const QString& text, int buttons,
                int defaultButton) override
    {
        Q_UNUSED(text)
        Q_UNUSED(buttons)
        confirmTitles << title;
        return confirmAnswers.isEmpty() ? defaultButton : confirmAnswers.takeFirst();
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
};

QTEST_MAIN(TestFlashEcuMitsuM32rCanDialog)
#include "flash_ecu_mitsu_m32r_can_dialog_test.moc"
