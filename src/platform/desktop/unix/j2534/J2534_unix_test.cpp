// Regression coverage for the cpp:S5276 narrowing fix in
// J2534::PassThruReadMsgs: the wire-parsed chunk length (`msg_byte_cnt`,
// `unsigned long`) is narrowed to the `uint32_t datalen` parameter of
// read_serial_data(). msg_byte_cnt is computed from adapter/ECU-supplied wire
// bytes (`received.at(3) - 1` in the hex-framed branches, or a parsed ASCII
// digit string in the 'm'/'y' branches) -- not from a value this process
// controls -- so a corrupted or malicious length byte can make it wrap to a
// huge 64-bit value before truncation. Pre-fix, that huge value silently
// truncated into the uint32_t datalen argument and PassThruReadMsgs pressed
// on as if nothing were wrong. Post-fix, PassThruReadMsgs rejects any
// msg_byte_cnt that would not fit in uint32_t with ERR_BUFFER_OVERFLOW before
// ever narrowing it.
//
// These drive the real byte-oriented protocol parser over a PTY (no
// hardware), matching the pattern in tests/tst_serial_port_crash.cpp.

#include <QtTest>
#include <QByteArray>

#if defined(__linux__)
#include <pty.h> // openpty
#else
#include <util.h> // openpty
#endif
#include <unistd.h> // read/write/close

#include "src/platform/desktop/unix/j2534/J2534_unix.h"

class J2534UnixTest : public QObject
{
    Q_OBJECT

  private slots:
    void passThruReadMsgs_withOversizedFrameLength_returnsBufferOverflow();
    void passThruReadMsgs_withNormalFrameLength_returnsSuccess();
};

void J2534UnixTest::passThruReadMsgs_withOversizedFrameLength_returnsBufferOverflow()
{
    int master = -1, slave = -1;
    char name[256] = {0};
    QVERIFY2(openpty(&master, &slave, name, nullptr, nullptr) == 0, "openpty failed");

    J2534 j2534;
    const QString ptyPath = QString::fromLocal8Bit(name);
    QCOMPARE(j2534.open_serial_port(ptyPath), ptyPath);

    // Craft a hex-framed chunk header: "ar" + '3' selects the
    // '3'/'4'/'5'/'6' branch of PassThruReadMsgs. The next two bytes it reads
    // are the frame's length byte (received.at(3)) and its message-type byte
    // (received.at(4)). A length byte of 0x00 makes
    // `msg_byte_cnt = received.at(3) - 1` compute -1, which -- assigned to
    // the unsigned long msg_byte_cnt -- wraps to ULONG_MAX. 0x10 is
    // TX_DONE_MSG (J2534_unix.h's private rx_msg_type enum), the simplest
    // msg_type branch that reaches the affected read_serial_data(msg_byte_cnt,
    // ...) call without needing any further scripted payload bytes.
    const unsigned char frame[] = {0x61, 0x72, 0x33, 0x00, 0x10};
    const auto written = ::write(master, frame, sizeof(frame));
    QVERIFY2(written == static_cast<ssize_t>(sizeof(frame)), "failed to write crafted frame to pty");

    PASSTHRU_MSG msg;
    unsigned long numMsgs = 0;
    // Short timeout: post-fix this returns immediately (the bounds check
    // fires before any attempt to read msg_byte_cnt bytes); pre-fix it would
    // truncate to a still-huge uint32_t and burn up to `timeout` waiting for
    // bytes that never arrive, then silently return STATUS_NOERROR.
    const long result = j2534.PassThruReadMsgs(1, &msg, &numMsgs, 50);

    QCOMPARE(result, static_cast<long>(ERR_BUFFER_OVERFLOW));

    j2534.close_serial_port();
    ::close(master);
}

void J2534UnixTest::passThruReadMsgs_withNormalFrameLength_returnsSuccess()
{
    // Control case: an in-range frame length must still be accepted and
    // processed exactly as before -- the bounds check must not disturb the
    // normal, in-range path.
    int master = -1, slave = -1;
    char name[256] = {0};
    QVERIFY2(openpty(&master, &slave, name, nullptr, nullptr) == 0, "openpty failed");

    J2534 j2534;
    const QString ptyPath = QString::fromLocal8Bit(name);
    QCOMPARE(j2534.open_serial_port(ptyPath), ptyPath);

    // Same TX_DONE_MSG framing as above, but with a length byte of 0x03, so
    // msg_byte_cnt = 3 - 1 = 2: a small, legitimate chunk length. The two
    // trailing bytes are the chunk payload read_serial_data(2, ...) consumes.
    const unsigned char frame[] = {0x61, 0x72, 0x33, 0x03, 0x10, 0xAA, 0xBB};
    const auto written = ::write(master, frame, sizeof(frame));
    QVERIFY2(written == static_cast<ssize_t>(sizeof(frame)), "failed to write crafted frame to pty");

    PASSTHRU_MSG msg;
    unsigned long numMsgs = 0;
    const long result = j2534.PassThruReadMsgs(1, &msg, &numMsgs, 50);

    QCOMPARE(result, static_cast<long>(STATUS_NOERROR));

    j2534.close_serial_port();
    ::close(master);
}

QTEST_GUILESS_MAIN(J2534UnixTest)
#include "J2534_unix_test.moc"
