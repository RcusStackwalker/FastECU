// Headless regression tests for the OpenPort/J2534 read-path crash.
//
// Crash report (macOS arm64): EXC_BAD_ACCESS, KERN_INVALID_ADDRESS at 0x8, in
//   QIODevice::isOpen()  <-  J2534::PassThruReadMsgs  <-  read_vbatt ...
// Register x0 == 0 at the fault: the QSerialPort* `serial` reached isOpen() is
// null. `serial` is only ever set by J2534's inline initialiser, so a null value
// means the J2534 object was torn down / used-after-free while a read was in
// flight (see project memory: reset_connection deletes j2534 while the event
// loop is pumped, and the read path has no null guard).
//
// These tests reproduce the exact fault deterministically by driving the read
// path with serial == nullptr. PRE-FIX they crash (SIGSEGV at 0x8); POST-FIX,
// once the read path null-guards `serial`, they return cleanly.

#include <QtTest>
#include <QByteArray>

#include "J2534_unix.h"

// `serial` is protected in J2534 so a test subclass can reproduce the torn-down
// state without adding any test-only method to the production class.
class TestableJ2534 : public J2534
{
public:
    void detachSerialPort() { serial = nullptr; }
};

class SerialPortCrashTest : public QObject
{
    Q_OBJECT

private slots:
    void isSerialPortOpen_withNullSerial_doesNotCrash();
    void readSerialData_withNullSerial_doesNotCrash();
    void passThruReadMsgs_withNullSerial_doesNotCrash();
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
    // This is the exact frame in the crash report: PassThruReadMsgs -> the read
    // path -> serial->isOpen(). Reaching the next line without crashing is the
    // assertion.
    j2534.PassThruReadMsgs(0, &msg, &numMsgs, 50);
    QVERIFY(true);
}

QTEST_GUILESS_MAIN(SerialPortCrashTest)
#include "tst_serial_port_crash.moc"
