#include <QtTest>
#include <QApplication>
#include <QQueue>
#include <QSignalSpy>
#include <QWidget>
#include "src/backend/definitions/kernelmemorymodels.h"
#include "src/backend/flash/flash_utils.h"
#include "src/backend/flash/ecu/flash_ecu_mitsu_m32r_can_operation.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_protocol.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_vendor_ext_protocol.h"
#include "src/algorithms/protocol/colt/qt_colt.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
#include "fake_backend.h"
#include "src/backend/definitions/file_actions.h"
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
        {
            return QByteArray();
        }
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
        QVERIFY2(!errSpy.empty(), "expected an LOG_E about the wrong ECU response");
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

    void testWriteCommand_currentlyPerformsBasicHandshakeOnly()
    {
        // Compatibility quirk: the operation has explicit read and write
        // branches, while test_write returns the successful basic-handshake
        // result without planning a write.
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);
        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x50\x81", 6));

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_128KB";
        QWidget dialog;
        FlashEcuMitsuM32rCanOperation op(&serial, &ecuCalDef, "test_write", &dialog);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));
        QCOMPARE(finishedSpy.at(0).at(0).toBool(), true);
        QCOMPARE(fake->takeCallLog().filter("write_echo_check:begin:"),
                 QStringList{"write_echo_check:begin:000007e01081"});
    }

    void writeFailsFastWhenRomTooSmallForTopRegion()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x50\x85", 6));
        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x67\x05\x00\x00\x00\x00", 10));
        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x67\x06", 6));

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FullRomData = QByteArray(int(MitsuColtCan::kTopRegionEnd) - 1, '\0'); // one byte short
        QWidget dialog;
        FlashEcuMitsuM32rCanOperation op(&serial, &ecuCalDef, "write", &dialog);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);
        QSignalSpy errSpy(&op, &FlashOperationWorker::LOG_E);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        QCOMPARE(writes.size(), 3); // handshake only -- no top-region read attempted
        QVERIFY2(!errSpy.empty(), "expected an LOG_E about the ROM being too small");
    }

    void writeChecksTopRegionBeforeUploadingStockRoutines()
    {
        QueuedFakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend *
                                 { fake = new QueuedFakeBackend(); return fake; });
        serial.set_add_ssm_header(false);

        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x50\x85", 6));
        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x67\x05\x00\x00\x00\x00", 10));
        fake->responses.enqueue(QByteArray("\x00\x00\x07\xE8\x67\x06", 6));
        // Deliberately malformed response to the very next request, so
        // readFlashRange's first chunk fails immediately -- this test only
        // needs to observe *which* request comes next, not complete a real
        // 128KB transfer (that's a bench-test concern, not a unit-test one).
        fake->responses.enqueue(QByteArray());

        FileActions::EcuCalDefStructure ecuCalDef;
        ecuCalDef.McuType = "M32R_384KB_1block";
        ecuCalDef.FullRomData = QByteArray(int(MitsuColtCan::kTopRegionEnd), '\0');
        QWidget dialog;
        FlashEcuMitsuM32rCanOperation op(&serial, &ecuCalDef, "write", &dialog);
        QSignalSpy finishedSpy(&op, &FlashOperationWorker::operationFinished);

        op.start();
        QVERIFY(finishedSpy.wait(2000));
        QVERIFY(op.wait(2000));

        QCOMPARE(finishedSpy.at(0).at(0).toBool(), false);
        const QStringList writes = fake->takeCallLog().filter("write_echo_check:begin:");
        QVERIFY2(writes.size() >= 4, "expected handshake (3) + top-region read (1)");
        // SID 0x23, addr 0x060000, len 0xc0 (192): buildReadMemoryByAddressFrame(kTopRegionStart, 192)
        QCOMPARE(writes.at(3), QString("write_echo_check:begin:000007e023060000c0"));
    }
};

int run_test_flash_ecu_mitsu_m32r_can_operation(int argc, char **argv)
{
    // FlashEcuMitsuM32rCanOperation's constructor takes a QWidget* dialog,
    // and the tests below construct a real QWidget -- that requires a
    // QApplication rather than a plain QCoreApplication (same fix as
    // test_flash_operation_worker.cpp's run function).
    QApplication app(argc, argv);
    TestFlashEcuMitsuM32rCanOperation t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_flash_ecu_mitsu_m32r_can_operation.moc"
