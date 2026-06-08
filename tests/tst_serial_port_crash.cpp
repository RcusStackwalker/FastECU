// Headless regression tests for the OpenPort/J2534 read-path crash.
//
// Crash report (macOS arm64): EXC_BAD_ACCESS, KERN_INVALID_ADDRESS at 0x8, in
//   QIODevice::isOpen()  <-  J2534::PassThruReadMsgs  <-  J2534::PassThruIoctl
//   <-  SerialPortActionsDirect::read_vbatt  <-  (reentrant flash-module read)
// Register x0 == 0 at the fault: the QSerialPort* `serial` reached isOpen() is
// null. `serial` is only ever set by J2534's inline initialiser, so a null value
// means the J2534 object was torn down / used-after-free while a read was in
// flight, and the read path has NO null/validity guard.
//
// These tests reproduce the exact fault deterministically and headlessly by
// driving the read path with serial == nullptr. PRE-FIX they SIGSEGV at 0x8;
// POST-FIX, once the read path null-guards `serial`, they return cleanly.
//
// Note on the use-after-free mechanism: reset_connection() does
//   delete j2534; delay(100) /* pumps the event loop */; j2534 = new J2534();
// but it clears use_openport2_adapter *before* the delete, so a reentrant
// read_vbatt() in that specific window is already gated off; and Qt purges
// posted events for a deleted QObject. A faithful deterministic headless
// reproduction of the live reentrancy is therefore impractical, so we reproduce
// the actual fault state (null serial) through both the low-level J2534 read
// entry points and the real read_vbatt -> PassThruIoctl -> PassThruReadMsgs chain.

#include <QtTest>
#include <QByteArray>

#include "J2534_unix.h"
#include "serial_port_actions_direct.h"

// `serial` is protected in J2534 so a test subclass can reproduce the torn-down
// state without adding any test-only method to the production class.
class TestableJ2534 : public J2534
{
public:
    void detachSerialPort() { serial = nullptr; }
};

// `j2534` is protected in SerialPortActionsDirect so the test can install a
// J2534 whose serial port is null and exercise the real read_vbatt() chain.
class TestableSerialPortActionsDirect : public SerialPortActionsDirect
{
public:
    TestableSerialPortActionsDirect() : SerialPortActionsDirect(nullptr) {}

    void installNullSerialJ2534()
    {
        delete j2534;
        TestableJ2534 *tj = new TestableJ2534();
        tj->detachSerialPort();
        j2534 = tj;
    }

    // Model the teardown reset_connection now performs: free j2534 and null the
    // pointer, so a reentrant read sees null (guarded) rather than a dangling ptr.
    void deleteAndNullJ2534() { delete j2534; j2534 = nullptr; }
};

class SerialPortCrashTest : public QObject
{
    Q_OBJECT

private slots:
    void isSerialPortOpen_withNullSerial_doesNotCrash();
    void readSerialData_withNullSerial_doesNotCrash();
    void passThruReadMsgs_withNullSerial_doesNotCrash();
    void readVbatt_throughNullJ2534Serial_doesNotCrash();
    void reentrantReadDuringTeardown_viaEventLoop_doesNotCrash();
};

void SerialPortCrashTest::isSerialPortOpen_withNullSerial_doesNotCrash()
{
    TestableJ2534 j2534;
    j2534.detachSerialPort();
    // J2534::is_serial_port_open() is `return serial->isOpen();` — the exact
    // null dereference from the crash report. Pre-fix: SIGSEGV at 0x8.
    QCOMPARE(j2534.is_serial_port_open(), false);
}

void SerialPortCrashTest::readSerialData_withNullSerial_doesNotCrash()
{
    TestableJ2534 j2534;
    j2534.detachSerialPort();
    // read_serial_data() opens with `if (serial->isOpen())` — the guard itself
    // dereferences the null serial.
    QCOMPARE(j2534.read_serial_data(3, 50), QByteArray());
}

void SerialPortCrashTest::passThruReadMsgs_withNullSerial_doesNotCrash()
{
    TestableJ2534 j2534;
    j2534.detachSerialPort();
    PASSTHRU_MSG msg;
    unsigned long numMsgs = 1;
    // The exact inner frame from the crash report: PassThruReadMsgs -> the read
    // path -> serial->isOpen(). Reaching the next line is the assertion.
    j2534.PassThruReadMsgs(0, &msg, &numMsgs, 50);
    QVERIFY(true);
}

void SerialPortCrashTest::readVbatt_throughNullJ2534Serial_doesNotCrash()
{
    TestableSerialPortActionsDirect spad;
    spad.installNullSerialJ2534();
    spad.use_openport2_adapter = true;   // take the J2534 branch in read_vbatt

    // Exercises the full crash call chain from the real entry point:
    // read_vbatt -> PassThruIoctl(READ_VBATT) -> read path -> serial->isOpen()
    // with serial == null. Pre-fix: SIGSEGV at 0x8. Post-fix: returns cleanly.
    spad.read_vbatt();
    QVERIFY(true);
}

void SerialPortCrashTest::reentrantReadDuringTeardown_viaEventLoop_doesNotCrash()
{
    // Reproduces the live reentrancy through a non-GUI event loop: FastECU pumps
    // processEvents() mid-operation (MainWindow::delay, reset_connection's
    // delay), so a still-alive consumer (a running flash module) can read vBatt
    // reentrantly while a teardown is freeing j2534.
    //
    // The queued read targets a SEPARATE, still-alive object, so Qt does not
    // purge it -> it fires from the event loop after teardown.
    TestableSerialPortActionsDirect spad;
    spad.use_openport2_adapter = true;   // read_vbatt takes the j2534 branch

    QObject consumer;   // mirrors the still-running flash module
    QMetaObject::invokeMethod(&consumer, [&spad]() { spad.read_vbatt(); },
                              Qt::QueuedConnection);

    // Teardown frees and nulls j2534 (as reset_connection now does) before the
    // queued read runs.
    spad.deleteAndNullJ2534();

    // Non-GUI event loop dispatches the reentrant read. Pre-fix (read_vbatt
    // dereferences j2534 unconditionally): SIGSEGV. Post-fix (read_vbatt guards
    // `use_openport2_adapter && j2534`): returns safely.
    QCoreApplication::processEvents();
    QVERIFY(true);
}

QTEST_GUILESS_MAIN(SerialPortCrashTest)
#include "tst_serial_port_crash.moc"
