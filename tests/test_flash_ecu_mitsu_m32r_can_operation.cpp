#include <QtTest>
#include <QApplication>
#include <QSignalSpy>
#include <QWidget>
#include "modules/ecu/flash_ecu_mitsu_m32r_can_operation.h"
#include "serial_port_actions.h"
#include "fake_backend.h"
#include "file_actions.h"
#include "test_flash_ecu_mitsu_m32r_can_operation.h"

class TestFlashEcuMitsuM32rCanOperation : public QObject
{
    Q_OBJECT
private slots:
    void connectFailure_wrongResponse_returnsFalseWithoutLooping()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(false);   // create backend
        fake->scriptedResponse = QByteArray();   // too short: connect_bootloader() rejects it

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_128KB";
        QWidget dialog;
        FlashEcuMitsuM32rCanOperation op(&serial, &ecuCalDef, "read", &dialog);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);
        QSignalSpy errSpy(&op, &FlashOperationWorker::LOG_E);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        QVERIFY2(errSpy.size() >= 1, "expected an LOG_E about the wrong ECU response");
    }

    void requestStop_duringReadLoop_stopsWithoutFinishingRead()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(false);
        // Valid diagnostic-session response for the "read" (basic) session:
        // build_request()'s header (00 00 07 E0) + service 0x50 + session 0x81.
        fake->scriptedResponse = QByteArray("\x00\x00\x07\xE0\x50\x81", 6);

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_128KB";
        QWidget dialog;
        FlashEcuMitsuM32rCanOperation op(&serial, &ecuCalDef, "read", &dialog);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        op.requestStop();   // requested immediately: at most one read-loop iteration runs
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        QVERIFY(ecuCalDef.FullRomData.isEmpty());   // read never completed
    }
};

int run_test_flash_ecu_mitsu_m32r_can_operation(int argc, char** argv) {
    // FlashEcuMitsuM32rCanOperation's constructor takes a QWidget* dialog,
    // and the tests below construct a real QWidget -- that requires a
    // QApplication rather than a plain QCoreApplication (same fix as
    // test_flash_operation_worker.cpp's run function).
    QApplication app(argc, argv);
    TestFlashEcuMitsuM32rCanOperation t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_flash_ecu_mitsu_m32r_can_operation.moc"
