#include <QtTest>
#include <cstdio>
#include <QApplication>
#include <QQueue>
#include <QSignalSpy>
#include <QWidget>
#include <kernelmemorymodels.h>
#include "modules/flash_utils.h"
#include "modules/ecu/flash_ecu_mitsu_m32r_can_operation.h"
#include "protocol/mitsu_colt_can_vendor_ext_protocol.h"
#include "serial_port_actions.h"
#include "fake_backend.h"
#include "file_actions.h"
#include "test_flash_ecu_mitsu_m32r_can_operation.h"

class QueuedFakeBackend : public FakeBackend
{
    Q_OBJECT
  public:
    QQueue<QByteArray> responses;

    QByteArray read_serial_data(uint16_t timeout) override
    {
        Q_UNUSED(timeout)
        if (responses.isEmpty())
            return QByteArray();
        return responses.dequeue();
    }
};

class TestFlashEcuMitsuM32rCanOperation : public QObject
{
    Q_OBJECT
  private slots:
    void colt384kReadRangeStartsAtUserspace()
    {
        const int index = FlashUtils::findFlashDeviceIndex("M32R_384KB_1block");

        QVERIFY(index >= 0);
        QCOMPARE(flashdevices[index].fblocks[0].start, uint32_t(0x00008000));
        QCOMPARE(flashdevices[index].fblocks[0].len, uint32_t(0x00058000));
    }

    void connectFailure_wrongResponse_returnsFalseWithoutLooping()
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(false);      // create backend
        fake->scriptedResponse = QByteArray(); // too short: connect_bootloader() rejects it

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
                                 [&fake]() -> SerialBackend *
                                 { fake = new FakeBackend(); return fake; });
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
        op.requestStop(); // requested immediately: at most one read-loop iteration runs
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        QVERIFY(ecuCalDef.FullRomData.isEmpty()); // read never completed
    }

    void vendorExtensionReadPerformsChallengeBeforeDiagnosticSession()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x63\x27\x41\x12\x34\x56\x78", 11));
        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x63\x27\x42", 7));
        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x50\x81", 6));

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_128KB";
        QWidget dialog;
        FlashEcuMitsuM32rCanOperation op(&serial, &ecuCalDef, "read", &dialog, true);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        op.requestStop();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        QVERIFY(writes.size() >= 3);
        QCOMPARE(writes.at(0), QString("write_echo_check:begin:000007e0232741"));

        const quint32 key = MitsuColtCanVendorExt::challengeInverseTransform(0x12345678);
        const QString expectedKeyFrame = QString("write_echo_check:begin:000007e0232742%1")
                                             .arg(QString::fromLatin1(MitsuColtCanVendorExt::keyToBytes(key).toHex()));
        QCOMPARE(writes.at(1), expectedKeyFrame);
        QCOMPARE(writes.at(2), QString("write_echo_check:begin:000007e01081"));
    }
};

int run_test_flash_ecu_mitsu_m32r_can_operation(int argc, char **argv)
{
    // FlashEcuMitsuM32rCanOperation's constructor takes a QWidget* dialog,
    // and the tests below construct a real QWidget -- that requires a
    // QApplication rather than a plain QCoreApplication (same fix as
    // test_flash_operation_worker.cpp's run function).
    fprintf(stderr, "[diag] before QApplication construction\n");
    fflush(stderr);
    QApplication app(argc, argv);
    fprintf(stderr, "[diag] after QApplication construction\n");
    fflush(stderr);
    TestFlashEcuMitsuM32rCanOperation t;
    fprintf(stderr, "[diag] before QTest::qExec\n");
    fflush(stderr);
    return QTest::qExec(&t, argc, argv);
}
#include "test_flash_ecu_mitsu_m32r_can_operation.moc"
