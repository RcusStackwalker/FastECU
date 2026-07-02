# Serial Backend Decoupling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restructure FastECU's serial layer so the adapter backend lives on a dedicated I/O thread behind an internal `SerialBackend` interface, with no `processEvents()` pumping anywhere in the backend, while `SerialPortActions`' public API stays unchanged for all ~30 consumer modules.

**Architecture:** `SerialPortActions` becomes a thin marshaling facade: every call is posted to a `SerialBackendHost`-owned I/O thread (`SerialIoThread`) and the caller blocks until completion (GUI-thread callers pump events in a single clearly-marked compat shim, deleted by spec 1b). `SerialPortActionsDirect` (fulfilling the spec's *DirectSerialBackend* role — the class is not renamed to avoid churn) and a new `RemoteSerialBackend` (QtRemoteObjects/WebSocket wrap) both implement `SerialBackend` and are constructed *on* the I/O thread, so `QSerialPort` affinity is correct by construction.

**Tech Stack:** Qt 6.11 (core, serialport, remoteobjects, websockets, testlib), qmake, C++17. Spec: `docs/superpowers/specs/2026-07-02-serial-backend-decoupling-design.md`.

## Global Constraints

- `SerialPortActions`' public method surface is unchanged; the only signature change is one *additive* constructor parameter with a default value (test backend factory).
- No changes to `serial_port/serial_port_actions.rep`, remote wire behavior, or `remote_utility/`.
- No changes to any consumer file: `mainwindow.*`, `ecu_operations.*`, `dtc_operations.*`, `dataterminal.*`, `hexcommander.*`, `modules/**`, `logging/**`, `protocol/**`.
- Class name `SerialPortActionsDirect` is kept (it fulfils the spec's `DirectSerialBackend` role).
- After this plan, `grep -rn "processEvents" serial_port/` must return exactly **one** hit: the compat shim in `serial_port_actions.h` (marked `COMPAT SHIM`).
- Existing suites must stay green: `tests/tests.pro`, `tests/serial_crash_tests.pro`, `tests/mut_dma_integration_tests.pro`.
- Build commands (run from the directory containing the `.pro`): `qmake6 <name>.pro && make` — qmake is `/opt/homebrew/bin/qmake` (macOS, Qt 6.11.1). Tests build in-tree (existing practice).
- `CONFIG += c++11` becomes `CONFIG += c++17` in `FastECU.pro` and `tests/tests.pro` (the new template helper uses `if constexpr`; Qt 6 needs C++17 anyway).
- Commit directly to `master` after each task (project convention — no feature branches).

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `serial_port/serial_backend.h` | create | Abstract `SerialBackend` interface — the internal seam |
| `serial_port/serial_backend_host.h/.cpp` | create | Owns the `SerialIoThread` + on-thread backend construction/teardown |
| `serial_port/remote_serial_backend.h/.cpp` | create | QtRO/WebSocket path moved out of the facade, implements `SerialBackend` |
| `serial_port/serial_port_actions.h/.cpp` | rewrite | Marshaling facade: `runOnBackend()` + compat shim; no direct/remote branching |
| `serial_port/serial_port_actions_direct.h/.cpp` | modify | Implements `SerialBackend`; all `processEvents()` removed |
| `serial_port/J2534_unix.cpp` | modify | Two `processEvents()` sites removed |
| `tests/mock_openport.h` | create | `MockOpenPort` (moved from crash tests) + `MockOpenPortThread` responder thread |
| `tests/tst_serial_port_crash.cpp` | modify | Uses threaded mock; reentrancy tests updated for no-pump semantics |
| `tests/fake_backend.h` | create | Scripted `SerialBackend` (subclass of direct) for facade tests |
| `tests/test_direct_backend.h/.cpp` | create | Interface roundtrip + backend behavior over PTY |
| `tests/test_facade_threading.h/.cpp` | create | Thread-safety suite: worker-thread calls, serialization, shim liveness |
| `tests/test_pty_e2e.h/.cpp` | create | Plain-serial end-to-end through facade + I/O thread from a worker thread |
| `tests/serial_backend_main.cpp`, `tests/serial_backend_tests.pro` | create | Runner for the three new suites |
| `FastECU.pro`, `tests/tests.pro`, `tests/mut_dma_integration_tests.pro`, `tests/serial_crash_tests.pro` | modify | New files wired in; C++17 |

---

### Task 1: Threaded MockOpenPort test infrastructure

The backend will stop pumping the event loop (Task 2). `MockOpenPort` in `tests/tst_serial_port_crash.cpp` is a `QSocketNotifier` on the *test* thread, so it only responds while the code under test pumps — after Task 2 it would go silent mid-`waitForReadyRead`. Move it to its own header and run it on a dedicated thread, so it responds like a real adapter regardless of what the calling thread does.

**Files:**
- Create: `tests/mock_openport.h`
- Modify: `tests/tst_serial_port_crash.cpp` (remove inlined `MockOpenPort`, use the threaded one)
- Modify: `tests/serial_crash_tests.pro` (add header)

**Interfaces:**
- Consumes: nothing new.
- Produces: `MockOpenPortThread(int masterFd)` — starts responding on construction; `std::atomic<bool> answerReadVbatt` member (direct cross-thread assignment); destructor joins. Tasks 2 and 7 use it.

- [ ] **Step 1: Create `tests/mock_openport.h`**

Move the `MockOpenPort` class body **verbatim** from `tests/tst_serial_port_crash.cpp` lines 41–92 into the new header, with exactly two changes: `bool answerReadVbatt = true;` becomes `std::atomic<bool> answerReadVbatt{true};`, and the class comment gains a line noting it is designed to run on `MockOpenPortThread`. Then append the thread wrapper:

```cpp
#ifndef MOCK_OPENPORT_H
#define MOCK_OPENPORT_H

#include <QObject>
#include <QSemaphore>
#include <QSocketNotifier>
#include <QThread>
#include <atomic>
#include <unistd.h>

// ... MockOpenPort moved verbatim here (see above) ...

// Runs a MockOpenPort on its own thread with its own event loop, so it
// responds like a real adapter regardless of whether the code under test
// pumps events. Responding starts before the constructor returns.
class MockOpenPortThread : public QThread
{
public:
    explicit MockOpenPortThread(int masterFd) : fd(masterFd)
    {
        start();
        ready.acquire();
    }
    ~MockOpenPortThread() override
    {
        quit();
        wait();
    }

    // Safe to flip from the test thread at any time (std::atomic member).
    MockOpenPort *mock = nullptr;

protected:
    void run() override
    {
        MockOpenPort m(fd);   // created here => notifier lives on this thread
        mock = &m;
        ready.release();
        exec();
        mock = nullptr;
    }

private:
    int fd;
    QSemaphore ready;
};

#endif // MOCK_OPENPORT_H
```

- [ ] **Step 2: Update `tests/tst_serial_port_crash.cpp`**

Delete the inlined `MockOpenPort` class (lines 41–92) and the now-unused `#include <QSocketNotifier>`. Add `#include "mock_openport.h"`. Replace each `MockOpenPort mock(master);` with `MockOpenPortThread mock(master);` (4 sites: `j2534Handshake_overMockPty_readVersionSucceeds`, `spadInitJ2534Connection_overMockPty_succeeds`, `loggingFlow_...`, `reentrantResetDuringInflightRead_...`). Replace `mock.answerReadVbatt = false;` with `mock.mock->answerReadVbatt = false;`.

- [ ] **Step 3: Add the header to `tests/serial_crash_tests.pro`**

```qmake
HEADERS += \
    mock_openport.h \
    ../serial_port/J2534_unix.h \
    ../serial_port/serial_port_actions_direct.h
```

- [ ] **Step 4: Run the crash suite — all pass (pump still present, mock now independent)**

Run: `cd tests && qmake6 serial_crash_tests.pro && make && ./serial_crash_tests`
Expected: all 8 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add tests/mock_openport.h tests/tst_serial_port_crash.cpp tests/serial_crash_tests.pro
git commit -m "test: run MockOpenPort on its own thread so it responds without event-loop pumping"
```

---

### Task 2: Remove processEvents() from the direct backend and J2534

Replace every event-loop pump in the backend with a wait that touches only the serial port itself (`waitForReadyRead`) or a plain sleep. TDD anchor: a new test asserting that a blocking read does **not** dispatch queued events — red before, green after.

**Files:**
- Modify: `tests/tst_serial_port_crash.cpp` (new red test + 2 reentrancy tests updated)
- Modify: `serial_port/serial_port_actions_direct.cpp` (8 pump sites)
- Modify: `serial_port/J2534_unix.cpp` (2 pump sites)

**Interfaces:**
- Consumes: `MockOpenPortThread` from Task 1.
- Produces: pump-free `SerialPortActionsDirect` / `J2534` — the serialization guarantee Tasks 5–7 rely on.

- [ ] **Step 1: Write the failing no-reentrancy test**

Add to `SerialPortCrashTest` (declare `void blockingRead_doesNotDispatchQueuedEvents();` in the private slots list, after the existing tests):

```cpp
void SerialPortCrashTest::blockingRead_doesNotDispatchQueuedEvents()
{
    // The old read paths pump QCoreApplication::processEvents() while waiting,
    // so ANY queued event — a timer, a user action, another serial call — runs
    // reentrantly in the middle of a read. Post-refactor the read must block
    // without dispatching foreign events.
    int master = -1, slave = -1;
    char name[256] = {0};
    QVERIFY2(openpty(&master, &slave, name, nullptr, nullptr) == 0, "openpty failed");
    // No responder: nothing arrives, the read waits out its full timeout.

    TestableSerialPortActionsDirect spad;
    spad.serial_port_prefix_linux = "";
    spad.serial_port_list = QStringList() << QString::fromLocal8Bit(name);
    QCOMPARE(spad.open_serial_port(), QString::fromLocal8Bit(name));

    bool dispatched = false;
    QMetaObject::invokeMethod(this, [&dispatched] { dispatched = true; },
                              Qt::QueuedConnection);

    spad.read_serial_data(200);

    QCOMPARE(dispatched, false);          // FAILS pre-fix: the pump ran the lambda
    QCoreApplication::processEvents();    // the event is still queued, not lost
    QCOMPARE(dispatched, true);
    ::close(master);
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `cd tests && qmake6 serial_crash_tests.pro && make && ./serial_crash_tests blockingRead_doesNotDispatchQueuedEvents`
Expected: FAIL at `QCOMPARE(dispatched, false)` — actual `true` (the pump dispatched the queued lambda).

- [ ] **Step 3: Replace the pumps**

In `serial_port/serial_port_actions_direct.cpp`, replace each of the six read/echo wait pumps (lines 711, 731, 759, 779, 804, 905 — all inside `read_serial_obd_data`, `read_serial_data`, `write_serial_data_echo_check`):

```cpp
// old
QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
// new — waits on this port's fd only; dispatches nothing else
serial->waitForReadyRead(1);
```

Replace the two delay bodies (lines 1691–1707):

```cpp
void SerialPortActionsDirect::fast_delay(int timeout)
{
    QThread::msleep(timeout);
}

void SerialPortActionsDirect::delay(int timeout)
{
    QThread::msleep(timeout);
}
```

(`accurate_delay` is a pure spin with no pump — leave it.) Add `#include <QThread>` to the cpp's include block if not already transitively present.

In `serial_port/J2534_unix.cpp`: line 89 (inside `J2534::read_serial_data`'s byte loop) becomes `serial->waitForReadyRead(1);`; the `J2534::delay` body (line 1086–1091) becomes `QThread::msleep(n);`. Add `#include <QThread>`. (`J2534_win.cpp` has no `processEvents` sites — verified.)

- [ ] **Step 4: Update the two pump-dependent crash tests**

`reentrantResetDuringInflightRead_overMockPty_doesNotCrash`: its premise (a queued `reset_connection` fires *mid-read* because the read pumps) is now impossible by construction. Rework it to assert the new ordering guarantee — rename to `resetQueuedDuringRead_runsAfterReadCompletes` (update the declaration too) and replace the tail after `mock.mock->answerReadVbatt = false;` with:

```cpp
    bool resetRan = false;
    QObject consumer;
    QMetaObject::invokeMethod(&consumer, [&] { spad.reset_connection(); resetRan = true; },
                              Qt::QueuedConnection);

    spad.read_vbatt();            // waits out its timeout; must NOT dispatch the reset
    QCOMPARE(resetRan, false);    // the queue no longer interleaves into reads

    QCoreApplication::processEvents();
    QCOMPARE(resetRan, true);     // the reset runs after, in order
```

Update the comment block above it to describe the new invariant (reads are atomic with respect to the event queue). `reentrantReadDuringTeardown_viaEventLoop_doesNotCrash` and `loggingFlow_...` call `QCoreApplication::processEvents()` explicitly *in test code* — they still exercise the null-guards; leave them unchanged.

- [ ] **Step 5: Run the full crash suite**

Run: `cd tests && qmake6 serial_crash_tests.pro && make && ./serial_crash_tests`
Expected: all 9 tests PASS (including the Step 1 test and the reworked reset-ordering test).

- [ ] **Step 6: Verify no pumps remain in the backend**

Run: `grep -rn "processEvents" serial_port/`
Expected: exactly one hit — `serial_port/serial_port_actions.cpp:122` (the facade's `delay()`, removed in Task 5).

- [ ] **Step 7: Commit**

```bash
git add serial_port/serial_port_actions_direct.cpp serial_port/J2534_unix.cpp tests/tst_serial_port_crash.cpp
git commit -m "refactor: replace event-loop pumping in the serial backend with port-local waits"
```

---

### Task 3: SerialBackend interface, implemented by SerialPortActionsDirect

**Files:**
- Create: `serial_port/serial_backend.h`
- Modify: `serial_port/serial_port_actions_direct.h` (inherit + accessor one-liners + `override` markers)
- Create: `tests/test_direct_backend.h`, `tests/test_direct_backend.cpp`
- Create: `tests/serial_backend_main.cpp`, `tests/serial_backend_tests.pro`
- Modify: `FastECU.pro` (HEADERS + `c++17`)

**Interfaces:**
- Consumes: nothing new.
- Produces: `class SerialBackend` — the exact contract below. Tasks 4–7 implement/consume it verbatim.

- [ ] **Step 1: Write the failing interface-roundtrip test**

`tests/test_direct_backend.h`:

```cpp
#ifndef TEST_DIRECT_BACKEND_H
#define TEST_DIRECT_BACKEND_H
int run_test_direct_backend(int argc, char **argv);
#endif
```

`tests/test_direct_backend.cpp`:

```cpp
#include "test_direct_backend.h"

#include <QtTest>
#include <thread>

#include <util.h>      // openpty (macOS)
#include <unistd.h>

#include "serial_backend.h"
#include "serial_port_actions_direct.h"

// Exercises the SerialBackend contract against the direct implementation
// purely through the base-class pointer: get/set roundtrips hit the same
// storage the backend's own I/O logic reads, and closed-port I/O calls
// return their documented empty/error values without hardware.
class TestDirectBackend : public QObject
{
    Q_OBJECT
private slots:
    void getSet_roundtrip_throughInterface();
    void closedPort_ioCalls_returnEmpty();
    // Backend behavior over a PTY (spec: reassembly, timeout, buffer
    // clearing, adapter-vanish). J2534 ioctl parameter handling stays with
    // the crash suite's MockOpenPort + the bench checklist.
    void ptyRead_reassemblesFragmentedFrame();
    void ptyRead_timesOutCleanOnSilence();
    void ptyClearRxBuffer_discardsPendingBytes();
    void ptyAdapterVanish_readReturnsCleanly();

private:
    // Opens a pty pair and the direct backend's plain-serial path on the
    // slave end. Returns the master fd; fills `backend`.
    int openPtyBackend(SerialPortActionsDirect &backend);
};

void TestDirectBackend::getSet_roundtrip_throughInterface()
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    b->set_add_ssm_header(true);
    QCOMPARE(b->get_add_ssm_header(), true);
    QCOMPARE(direct.add_ssm_header, true);   // same storage the I/O paths read

    b->set_serial_port_baudrate("10400");
    QCOMPARE(b->get_serial_port_baudrate(), QString("10400"));
    QCOMPARE(direct.serial_port_baudrate, QString("10400"));

    b->set_kline_startbyte(0x80);
    QCOMPARE(b->get_kline_startbyte(), (uint8_t)0x80);

    b->set_can_source_address(0x7E0);
    QCOMPARE(b->get_can_source_address(), (uint32_t)0x7E0);

    b->set_serial_port_list(QStringList() << "ttyUSB0");
    QCOMPARE(b->get_serial_port_list(), QStringList() << "ttyUSB0");

    QCOMPARE(b->qobject(), static_cast<QObject *>(&direct));
}

void TestDirectBackend::closedPort_ioCalls_returnEmpty()
{
    SerialPortActionsDirect direct;
    SerialBackend *b = &direct;

    QCOMPARE(b->is_serial_port_open(), false);
    QCOMPARE(b->read_serial_data(50), QByteArray());
    // write_serial_data's `return STATUS_SUCCESS;` converts int 0 through the
    // QByteArray(const char*) ctor => empty array. Pin today's behavior.
    QCOMPARE(b->write_serial_data(QByteArray("\x01\x02", 2)), QByteArray());
    b->waitForSource();                // default no-op must not block or crash
}

int TestDirectBackend::openPtyBackend(SerialPortActionsDirect &backend)
{
    int master = -1, slave = -1;
    char name[256] = {0};
    if (openpty(&master, &slave, name, nullptr, nullptr) != 0)
        return -1;
    backend.serial_port_prefix_linux = "";
    backend.serial_port_list = QStringList() << QString::fromLocal8Bit(name);
    if (backend.open_serial_port() != QString::fromLocal8Bit(name))
        return -1;
    return master;
}

void TestDirectBackend::ptyRead_reassemblesFragmentedFrame()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    // The "ECU" delivers one framed message in two fragments with a gap:
    // header first, payload+checksum 30ms later. The reader must reassemble.
    std::thread responder([master] {
        ::write(master, "\x80\xf0\x10\x02", 4);
        QThread::msleep(30);
        ::write(master, "\xaa\xbb\xcc", 3);
    });
    const QByteArray got = direct.read_serial_data(500);
    responder.join();
    QCOMPARE(got, QByteArray("\x80\xf0\x10\x02\xaa\xbb\xcc", 7));
    ::close(master);
}

void TestDirectBackend::ptyRead_timesOutCleanOnSilence()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    QElapsedTimer t;
    t.start();
    QCOMPARE(direct.read_serial_data(150), QByteArray());
    const qint64 elapsed = t.elapsed();
    QVERIFY2(elapsed >= 140 && elapsed < 1000,
             qPrintable(QString("timeout took %1 ms").arg(elapsed)));
    ::close(master);
}

void TestDirectBackend::ptyClearRxBuffer_discardsPendingBytes()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    ::write(master, "\x11\x22\x33", 3);          // junk arrives...
    QThread::msleep(50);                          // ...and lands in the buffer
    direct.clear_rx_buffer();                     // must discard it
    QCOMPARE(direct.read_serial_data(100), QByteArray());
    ::close(master);
}

void TestDirectBackend::ptyAdapterVanish_readReturnsCleanly()
{
    SerialPortActionsDirect direct;
    const int master = openPtyBackend(direct);
    QVERIFY(master >= 0);

    ::close(master);                              // the adapter disappears
    // The read must come back empty (possibly via handle_error ->
    // reset_connection) without crashing or hanging.
    QCOMPARE(direct.read_serial_data(100), QByteArray());
}

int run_test_direct_backend(int argc, char **argv)
{
    TestDirectBackend t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_direct_backend.moc"
```

`tests/serial_backend_main.cpp`:

```cpp
#include <QCoreApplication>
#include "test_direct_backend.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    int status = 0;
    status |= run_test_direct_backend(argc, argv);
    return status;
}
```

`tests/serial_backend_tests.pro`:

```qmake
# Backend/adapter-level and thread-safety tests for the serial backend
# decoupling (docs/superpowers/specs/2026-07-02-serial-backend-decoupling-design.md).
# Run with: qmake6 serial_backend_tests.pro && make && ./serial_backend_tests
QT += core testlib serialport remoteobjects websockets
CONFIG += c++17 console
CONFIG -= app_bundle
include(../hardening.pri)

TARGET = serial_backend_tests
INCLUDEPATH += .. ../serial_port

REPC_REPLICA = ../serial_port/serial_port_actions.rep

unix {
    SOURCES += ../serial_port/J2534_unix.cpp
    HEADERS += ../serial_port/J2534_unix.h
}

SOURCES += \
    serial_backend_main.cpp \
    test_direct_backend.cpp \
    ../serial_port/serial_port_actions_direct.cpp

HEADERS += \
    mock_openport.h \
    test_direct_backend.h \
    ../serial_port/serial_backend.h \
    ../serial_port/serial_port_actions_direct.h
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd tests && qmake6 serial_backend_tests.pro && make`
Expected: compile FAILURE — `serial_backend.h` does not exist / `SerialPortActionsDirect` has no `get_add_ssm_header`.

- [ ] **Step 3: Create `serial_port/serial_backend.h`** (the complete contract)

```cpp
#ifndef SERIAL_BACKEND_H
#define SERIAL_BACKEND_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <stdint.h>

class QObject;

// Internal seam between the SerialPortActions facade and the two adapter
// backends (SerialPortActionsDirect, RemoteSerialBackend). Consumers never
// see this type. Every method executes on the SerialIoThread — implementations
// may block, but must never pump the application event loop.
//
// Method names/signatures deliberately mirror the facade's public surface so
// the facade forwards 1:1. Getter/setter pairs correspond to the direct
// backend's public config fields.
class SerialBackend
{
public:
    virtual ~SerialBackend() = default;

    // The QObject identity of the concrete backend, used by the facade to
    // wire LOG_* / stateChanged signal forwarding.
    virtual QObject *qobject() = 0;

    // -- config get/set pairs (44) --------------------------------------
    virtual bool get_serialPortAvailable() = 0;
    virtual bool set_serialPortAvailable(bool value) = 0;
    virtual bool get_setRequestToSend() = 0;
    virtual bool set_setRequestToSend(bool value) = 0;
    virtual bool get_setDataTerminalReady() = 0;
    virtual bool set_setDataTerminalReady(bool value) = 0;
    virtual bool get_add_ssm_header() = 0;
    virtual bool set_add_ssm_header(bool value) = 0;
    virtual bool get_add_iso9141_header() = 0;
    virtual bool set_add_iso9141_header(bool value) = 0;
    virtual bool get_add_iso14230_header() = 0;
    virtual bool set_add_iso14230_header(bool value) = 0;
    virtual bool get_is_iso14230_connection() = 0;
    virtual bool set_is_iso14230_connection(bool value) = 0;
    virtual bool get_is_can_connection() = 0;
    virtual bool set_is_can_connection(bool value) = 0;
    virtual bool get_is_iso15765_connection() = 0;
    virtual bool set_is_iso15765_connection(bool value) = 0;
    virtual bool get_is_29_bit_id() = 0;
    virtual bool set_is_29_bit_id(bool value) = 0;
    virtual bool get_use_openport2_adapter() = 0;
    virtual bool set_use_openport2_adapter(bool value) = 0;

    virtual int  get_requestToSendEnabled() = 0;
    virtual bool set_requestToSendEnabled(int value) = 0;
    virtual int  get_requestToSendDisabled() = 0;
    virtual bool set_requestToSendDisabled(int value) = 0;
    virtual int  get_dataTerminalEnabled() = 0;
    virtual bool set_dataTerminalEnabled(int value) = 0;
    virtual int  get_dataTerminalDisabled() = 0;
    virtual bool set_dataTerminalDisabled(int value) = 0;

    virtual uint8_t get_kline_startbyte() = 0;
    virtual bool    set_kline_startbyte(uint8_t value) = 0;
    virtual uint8_t get_kline_tester_id() = 0;
    virtual bool    set_kline_tester_id(uint8_t value) = 0;
    virtual uint8_t get_kline_target_id() = 0;
    virtual bool    set_kline_target_id(uint8_t value) = 0;
    virtual uint8_t get_serial_port_parity() = 0;
    virtual bool    set_serial_port_parity(uint8_t parity) = 0;

    virtual QByteArray get_ssm_receive_header_start() = 0;
    virtual bool       set_ssm_receive_header_start(QByteArray value) = 0;

    virtual QStringList get_serial_port_list() = 0;
    virtual bool        set_serial_port_list(QStringList value) = 0;

    virtual QString get_openedSerialPort() = 0;
    virtual bool    set_openedSerialPort(QString value) = 0;
    virtual QString get_subaru_02_16bit_bootloader_baudrate() = 0;
    virtual bool    set_subaru_02_16bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_04_16bit_bootloader_baudrate() = 0;
    virtual bool    set_subaru_04_16bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_02_32bit_bootloader_baudrate() = 0;
    virtual bool    set_subaru_02_32bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_04_32bit_bootloader_baudrate() = 0;
    virtual bool    set_subaru_04_32bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_05_32bit_bootloader_baudrate() = 0;
    virtual bool    set_subaru_05_32bit_bootloader_baudrate(QString value) = 0;
    virtual QString get_subaru_02_16bit_kernel_baudrate() = 0;
    virtual bool    set_subaru_02_16bit_kernel_baudrate(QString value) = 0;
    virtual QString get_subaru_04_16bit_kernel_baudrate() = 0;
    virtual bool    set_subaru_04_16bit_kernel_baudrate(QString value) = 0;
    virtual QString get_subaru_02_32bit_kernel_baudrate() = 0;
    virtual bool    set_subaru_02_32bit_kernel_baudrate(QString value) = 0;
    virtual QString get_subaru_04_32bit_kernel_baudrate() = 0;
    virtual bool    set_subaru_04_32bit_kernel_baudrate(QString value) = 0;
    virtual QString get_subaru_05_32bit_kernel_baudrate() = 0;
    virtual bool    set_subaru_05_32bit_kernel_baudrate(QString value) = 0;
    virtual QString get_can_speed() = 0;
    virtual bool    set_can_speed(QString value) = 0;
    virtual QString get_serial_port_baudrate() = 0;
    virtual bool    set_serial_port_baudrate(QString value) = 0;
    virtual QString get_serial_port_linux() = 0;
    virtual bool    set_serial_port_linux(QString value) = 0;
    virtual QString get_serial_port_windows() = 0;
    virtual bool    set_serial_port_windows(QString value) = 0;
    virtual QString get_serial_port() = 0;
    virtual bool    set_serial_port(QString value) = 0;
    virtual QString get_serial_port_prefix() = 0;
    virtual bool    set_serial_port_prefix(QString value) = 0;
    virtual QString get_serial_port_prefix_linux() = 0;
    virtual bool    set_serial_port_prefix_linux(QString value) = 0;
    virtual QString get_serial_port_prefix_win() = 0;
    virtual bool    set_serial_port_prefix_win(QString value) = 0;

    virtual uint32_t get_can_source_address() = 0;
    virtual bool     set_can_source_address(uint32_t value) = 0;
    virtual uint32_t get_can_destination_address() = 0;
    virtual bool     set_can_destination_address(uint32_t value) = 0;
    virtual uint32_t get_iso15765_source_address() = 0;
    virtual bool     set_iso15765_source_address(uint32_t value) = 0;
    virtual uint32_t get_iso15765_destination_address() = 0;
    virtual bool     set_iso15765_destination_address(uint32_t value) = 0;

    // -- operations ------------------------------------------------------
    virtual bool is_serial_port_open() = 0;
    virtual int change_port_speed(QString portSpeed) = 0;
    virtual bool set_kline_timings(uint32_t parameter, int value) = 0;
    virtual int set_j2534_ioctl(uint32_t parameter, int value) = 0;
    virtual QByteArray five_baud_init(QByteArray output) = 0;
    virtual int fast_init(QByteArray output) = 0;
    virtual int set_lec_lines(int lec1, int lec2) = 0;
    virtual int pulse_lec_1_line(int timeout) = 0;
    virtual int pulse_lec_2_line(int timeout) = 0;
    virtual void reset_connection() = 0;
    virtual QByteArray read_serial_obd_data(uint16_t timeout) = 0;
    virtual QByteArray read_serial_data(uint16_t timeout) = 0;
    virtual QByteArray write_serial_data(QByteArray output) = 0;
    virtual QByteArray write_serial_data_echo_check(QByteArray output) = 0;
    virtual bool get_is_tx_done() = 0;
    virtual int clear_rx_buffer() = 0;
    virtual int clear_tx_buffer() = 0;
    virtual int send_periodic_j2534_data(QByteArray output, int timeout) = 0;
    virtual int stop_periodic_j2534_data() = 0;
    virtual QStringList check_serial_ports() = 0;
    virtual QString open_serial_port() = 0;
    virtual unsigned long read_vbatt() = 0;

    // Remote-only: block until the QtRO source is replicated. No-op for the
    // direct backend.
    virtual void waitForSource() {}
};

#endif // SERIAL_BACKEND_H
```

- [ ] **Step 4: Make `SerialPortActionsDirect` implement it**

In `serial_port/serial_port_actions_direct.h`: add `#include "serial_backend.h"`, change the class declaration to `class SerialPortActionsDirect : public QObject, public SerialBackend`, and mark the already-matching operation signatures (`is_serial_port_open` … `read_vbatt` — every name in the interface's operations block except `set_j2534_ioctl`, `qobject`, and `waitForSource`) with `override`. Then add, in the public section:

```cpp
    // -- SerialBackend ----------------------------------------------------
    QObject *qobject() override { return this; }
    // The interface takes uint32_t; the long-standing member takes unsigned
    // long. Distinct overloads on LP64 — adapt explicitly.
    int set_j2534_ioctl(uint32_t parameter, int value) override
    { return set_j2534_ioctl((unsigned long)parameter, value); }

    bool get_serialPortAvailable() override { return serialPortAvailable; }
    bool set_serialPortAvailable(bool value) override { serialPortAvailable = value; return true; }
```

…and the remaining 43 get/set pairs following exactly that two-line pattern (`TYPE get_NAME() override { return FIELD; }` / `bool set_NAME(TYPE value) override { FIELD = value; return true; }`). The `NAME`→`FIELD` mapping is the identity — every accessor name minus its `get_`/`set_` prefix is the field name (`setRequestToSend`, `setDataTerminalReady`, `add_ssm_header`, `add_iso9141_header`, `add_iso14230_header`, `is_iso14230_connection`, `is_can_connection`, `is_iso15765_connection`, `is_29_bit_id`, `use_openport2_adapter`, `requestToSendEnabled`, `requestToSendDisabled`, `dataTerminalEnabled`, `dataTerminalDisabled`, `kline_startbyte`, `kline_tester_id`, `kline_target_id`, `serial_port_parity`, `ssm_receive_header_start`, `serial_port_list`, `openedSerialPort`, the ten `subaru_*_baudrate` fields, `can_speed`, `serial_port_baudrate`, `serial_port_linux`, `serial_port_windows`, `serial_port`, `serial_port_prefix`, `serial_port_prefix_linux`, `serial_port_prefix_win`, `can_source_address`, `can_destination_address`, `iso15765_source_address`, `iso15765_destination_address`). Types come from the interface header — the compiler enforces completeness (abstract until all 44 pairs exist).

- [ ] **Step 5: Run the new suite and the regression suites**

Run: `cd tests && qmake6 serial_backend_tests.pro && make && ./serial_backend_tests`
Expected: 6 tests PASS.
Run: `cd tests && qmake6 serial_crash_tests.pro && make && ./serial_crash_tests`
Expected: all PASS (the direct class gained a base and accessors; behavior unchanged).

- [ ] **Step 6: Wire into the app build**

In `FastECU.pro`: change line 6 `CONFIG += c++11` → `CONFIG += c++17`; add `serial_port/serial_backend.h \` to the HEADERS block (before `serial_port/serial_port_actions.h`).
Run: `qmake6 FastECU.pro && make -j8`
Expected: clean build of the app binary.

- [ ] **Step 7: Commit**

```bash
git add serial_port/serial_backend.h serial_port/serial_port_actions_direct.h FastECU.pro tests/test_direct_backend.h tests/test_direct_backend.cpp tests/serial_backend_main.cpp tests/serial_backend_tests.pro
git commit -m "feat: add SerialBackend interface, implemented by SerialPortActionsDirect"
```

---

### Task 4: RemoteSerialBackend

Move the QtRO/WebSocket plumbing out of the facade into a `SerialBackend` implementation. In this task the class is created and unit-smoke-tested standalone; the facade keeps its old code until Task 5 deletes it (temporary duplication, one task long).

**Files:**
- Create: `serial_port/remote_serial_backend.h`, `serial_port/remote_serial_backend.cpp`
- Modify: `FastECU.pro`, `tests/serial_backend_tests.pro`
- Create test: append a suite to `tests/test_direct_backend.cpp`? No — create `tests/test_remote_backend_smoke.h/.cpp` (construction/teardown only; the spec accepts that remote has no automated call tests)

**Interfaces:**
- Consumes: `SerialBackend` (Task 3).
- Produces: `RemoteSerialBackend(QString peerAddress, QString password, QWebSocket *externalSocket = nullptr, QObject *parent = nullptr)`; must be constructed on the thread that will run it. Signals: `LOG_E/W/I/D(QString,bool,bool)`, `stateChanged(QRemoteObjectReplica::State,QRemoteObjectReplica::State)`.

- [ ] **Step 1: Write the failing smoke test**

`tests/test_remote_backend_smoke.h`:

```cpp
#ifndef TEST_REMOTE_BACKEND_SMOKE_H
#define TEST_REMOTE_BACKEND_SMOKE_H
int run_test_remote_backend_smoke(int argc, char **argv);
#endif
```

`tests/test_remote_backend_smoke.cpp`:

```cpp
#include "test_remote_backend_smoke.h"

#include <QtTest>
#include "remote_serial_backend.h"

// The remote path has no automated call-level tests (spec risk note: kept a
// strictly mechanical wrap + manual smoke test before release). This suite
// pins the only things that can be checked headlessly: construction against
// an unreachable peer neither blocks nor crashes, and teardown is clean.
class TestRemoteBackendSmoke : public QObject
{
    Q_OBJECT
private slots:
    void constructAndDestroy_localPeer_noBlockNoCrash();
};

void TestRemoteBackendSmoke::constructAndDestroy_localPeer_noBlockNoCrash()
{
    QElapsedTimer t;
    t.start();
    {
        RemoteSerialBackend remote("local:fastecu-test-nonexistent", "pw");
        QVERIFY(remote.qobject() != nullptr);
    }
    QVERIFY2(t.elapsed() < 2000, "construction/teardown must not block");
}

int run_test_remote_backend_smoke(int argc, char **argv)
{
    TestRemoteBackendSmoke t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_remote_backend_smoke.moc"
```

Add to `tests/serial_backend_main.cpp`: include `test_remote_backend_smoke.h` and `status |= run_test_remote_backend_smoke(argc, argv);`.
Add to `tests/serial_backend_tests.pro`: `test_remote_backend_smoke.cpp` to SOURCES, `test_remote_backend_smoke.h` to HEADERS, and the new backend files:

```qmake
SOURCES += ../serial_port/remote_serial_backend.cpp \
           ../serial_port/websocketiodevice.cpp
HEADERS += ../serial_port/remote_serial_backend.h \
           ../serial_port/websocketiodevice.h
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd tests && qmake6 serial_backend_tests.pro && make`
Expected: compile FAILURE — `remote_serial_backend.h` missing.

- [ ] **Step 3: Create `serial_port/remote_serial_backend.h`**

```cpp
#ifndef REMOTE_SERIAL_BACKEND_H
#define REMOTE_SERIAL_BACKEND_H

#include <QObject>
#include <QtRemoteObjects/qremoteobjectnode.h>

#include "serial_backend.h"
#include "websocketiodevice.h"

class SerialPortActionsRemoteReplica;

// SerialBackend implementation for the remote (QtRemoteObjects over
// WebSocket, or local socket) adapter path. Strictly a mechanical wrap of
// the replica calls that used to live inline in SerialPortActions — no wire
// behavior change.
//
// Must be constructed ON the thread that will run it (SerialIoThread): the
// node, websocket, and replica all take that thread's affinity.
//
// Blocking-wait caveat, accepted by the spec: qtrohelper::slot_sync() runs a
// local event loop while waiting for the reply, so — exactly like today's
// GUI-thread behavior — other queued backend calls can interleave on the
// remote path. The strict no-interleave guarantee applies to the direct
// backend only.
class RemoteSerialBackend : public QObject, public SerialBackend
{
    Q_OBJECT

signals:
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

public:
    explicit RemoteSerialBackend(QString peerAddress,
                                 QString password,
                                 QWebSocket *externalSocket = nullptr,
                                 QObject *parent = nullptr);
    ~RemoteSerialBackend() override;

    QObject *qobject() override { return this; }
    void waitForSource() override;

    // All 44 get/set pairs and all 22 operations from SerialBackend, each
    // declared with override — signatures exactly as in serial_backend.h.
    // (Declarations elided here for brevity; the compiler enforces the set.)

private:
    QString peerAddress;
    QString password;

    const QString autodiscoveryMessage = "FastECU_PTP_Autodiscovery";
    const QString remoteObjectName = "FastECU";
    const QString wssPath = "/" + remoteObjectName;
    const QString webSocketPasswordHeader = "fastecu-basic-password";
    const int heartbeatInterval = 0;

    QWebSocket *webSocket = nullptr;
    WebSocketIoDevice *socket = nullptr;
    QRemoteObjectNode node;
    SerialPortActionsRemoteReplica *serial_remote = nullptr;

    void startRemote();
    void startOverNetwork();
    void startLocal();
    void sendAutoDiscoveryMessage();

private slots:
    void websocket_connected();
    void serialRemoteStateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);
};

#endif // REMOTE_SERIAL_BACKEND_H
```

In the real header, write out all the `override` declarations the comment refers to (copy the method list from `serial_backend.h`, replacing `= 0` with `override;`).

- [ ] **Step 4: Create `serial_port/remote_serial_backend.cpp`**

Constructor / plumbing (adapted from `serial_port_actions.cpp` lines 6–132 — same logic, renamed `startOverNetwok`→`startOverNetwork`, and `delay()` replaced):

```cpp
#include "remote_serial_backend.h"

#include <QThread>
#include <QWebSocket>
#include "qtrohelper.hpp"
#include "rep_serial_port_actions_replica.h"

RemoteSerialBackend::RemoteSerialBackend(QString peerAddress, QString password,
                                         QWebSocket *externalSocket, QObject *parent)
    : QObject{parent}
    , peerAddress(peerAddress)
    , password(password)
{
    if (externalSocket && externalSocket->thread() != thread())
    {
        emit LOG_W("RemoteSerialBackend: external websocket has foreign thread "
                   "affinity, creating own socket instead", true, true);
        externalSocket = nullptr;
    }
    webSocket = externalSocket ? externalSocket
                               : new QWebSocket("", QWebSocketProtocol::VersionLatest, this);
    socket = new WebSocketIoDevice(webSocket, webSocket);
    startRemote();
}

RemoteSerialBackend::~RemoteSerialBackend()
{
}

void RemoteSerialBackend::waitForSource()
{
    while (!serial_remote->waitForSource(10000))
    {
        sendAutoDiscoveryMessage();
        QThread::msleep(50);
        emit LOG_D("RemoteSerialBackend: Waiting for remote peer...", true, true);
    }
}
```

`startRemote`/`startOverNetwork`/`startLocal`/`sendAutoDiscoveryMessage`/`websocket_connected`/`serialRemoteStateChanged` bodies move **verbatim** from `serial_port_actions.cpp` (lines 42–105 and 125–132), with `this` context adjusted (they become members of this class; the `stateChanged` forward stays).

Every `SerialBackend` method is the current facade's **else-branch body**, moved verbatim from `serial_port_actions.cpp`. Worked examples of each shape:

```cpp
bool RemoteSerialBackend::get_add_ssm_header()
{
    return qtrohelper::slot_sync(serial_remote->get_add_ssm_header());
}

bool RemoteSerialBackend::set_add_ssm_header(bool value)
{
    return qtrohelper::slot_sync(serial_remote->set_add_ssm_header(value));
}

QByteArray RemoteSerialBackend::read_serial_data(uint16_t timeout)
{
    return qtrohelper::slot_sync(serial_remote->read_serial_data(timeout));
}

void RemoteSerialBackend::reset_connection()
{
    qtrohelper::slot_sync(serial_remote->reset_connection());   // bool result discarded
}

bool RemoteSerialBackend::set_kline_timings(uint32_t parameter, int value)
{
    Q_UNUSED(parameter); Q_UNUSED(value);
    return true;   // facade never forwarded this to the remote (see old lines 885-891)
}

bool RemoteSerialBackend::get_is_tx_done()
{
    // The old facade fell off the end of a non-void function here (UB).
    // The .rep has no get_is_tx_done; report tx done so callers don't spin.
    return true;
}
```

Apply the `slot_sync` one-liner pattern to all remaining pairs/operations (the replica slot names match the interface names 1:1; the bodies to move are the `else` branches of `serial_port_actions.cpp` lines 134–1168).

- [ ] **Step 5: Add to `FastECU.pro`**

`serial_port/remote_serial_backend.cpp` to SOURCES (after `serial_port/serial_port_actions_direct.cpp`), `serial_port/remote_serial_backend.h` to HEADERS.

- [ ] **Step 6: Run tests and app build**

Run: `cd tests && qmake6 serial_backend_tests.pro && make && ./serial_backend_tests`
Expected: 7 tests PASS (6 direct + 1 remote smoke).
Run: `qmake6 FastECU.pro && make -j8`
Expected: clean build.

- [ ] **Step 7: Commit**

```bash
git add serial_port/remote_serial_backend.h serial_port/remote_serial_backend.cpp FastECU.pro tests/test_remote_backend_smoke.h tests/test_remote_backend_smoke.cpp tests/serial_backend_main.cpp tests/serial_backend_tests.pro
git commit -m "feat: add RemoteSerialBackend wrapping the QtRO/WebSocket path behind SerialBackend"
```

---

### Task 5: SerialBackendHost + facade rewrite

The heart of the refactor: the I/O thread host, the `runOnBackend()` marshal with the compat shim, and the wholesale rewrite of `SerialPortActions` to a single-path facade.

**Files:**
- Create: `serial_port/serial_backend_host.h`, `serial_port/serial_backend_host.cpp`
- Rewrite: `serial_port/serial_port_actions.h`, `serial_port/serial_port_actions.cpp`
- Create: `tests/fake_backend.h`, `tests/test_facade_threading.h`, `tests/test_facade_threading.cpp`
- Modify: `FastECU.pro`, `tests/serial_backend_tests.pro`, `tests/tests.pro`, `tests/mut_dma_integration_tests.pro`

**Interfaces:**
- Consumes: `SerialBackend` (Task 3), `SerialPortActionsDirect` (Task 3), `RemoteSerialBackend` (Task 4).
- Produces: `SerialBackendHost` (`createBackend(std::function<SerialBackend*()>)`, `context()`, `ioThread()`); `SerialPortActions` constructor gains trailing `std::function<SerialBackend*()> backendFactoryForTests = {}`; `FakeBackend` (subclass of `SerialPortActionsDirect` with `callLog`, `readDelayMs`, `scriptedResponse`). Tasks 6–7 use all three.

- [ ] **Step 1: Write the failing facade tests**

`tests/fake_backend.h`:

```cpp
#ifndef FAKE_BACKEND_H
#define FAKE_BACKEND_H

#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>

#include "serial_port_actions_direct.h"

// Scripted backend for facade-level tests. Subclasses the direct backend so
// the 44 config accessors come for free (real fields, no port ever opened);
// overrides the I/O entry points with scripted, observable behavior.
class FakeBackend : public SerialPortActionsDirect
{
    Q_OBJECT
public:
    int readDelayMs = 0;              // set before use; simulates a slow adapter
    QByteArray scriptedResponse;

    QStringList takeCallLog()
    {
        QMutexLocker l(&logMutex);
        QStringList out = callLog;
        callLog.clear();
        return out;
    }

    bool is_serial_port_open() override { return true; }

    QByteArray read_serial_data(uint16_t timeout) override
    {
        log(QString("read:begin:t=%1").arg(timeout));
        if (readDelayMs)
            QThread::msleep(readDelayMs);
        log("read:end");
        return scriptedResponse;
    }

    QByteArray write_serial_data(QByteArray output) override
    {
        log("write:begin:" + QString::fromLatin1(output.toHex()));
        if (readDelayMs)
            QThread::msleep(readDelayMs);
        log("write:end");
        return QByteArray();   // matches the real backend's empty return
    }

private:
    void log(const QString &s)
    {
        QMutexLocker l(&logMutex);
        callLog.append(s);
    }
    QMutex logMutex;
    QStringList callLog;
};

#endif // FAKE_BACKEND_H
```

`tests/test_facade_threading.h`:

```cpp
#ifndef TEST_FACADE_THREADING_H
#define TEST_FACADE_THREADING_H
int run_test_facade_threading(int argc, char **argv);
#endif
```

`tests/test_facade_threading.cpp` (first three tests now; Task 6 appends the rest):

```cpp
#include "test_facade_threading.h"

#include <QtTest>
#include <QSemaphore>
#include <thread>

#include "fake_backend.h"
#include "serial_port_actions.h"

class TestFacadeThreading : public QObject
{
    Q_OBJECT
private slots:
    void constructDestroy_withoutUse_noThreadNoHang();
    void getSet_marshalsToBackendThread();
    void scriptedRead_returnsThroughFacade();
};

void TestFacadeThreading::constructDestroy_withoutUse_noThreadNoHang()
{
    QElapsedTimer t;
    t.start();
    {
        SerialPortActions serial;   // never used: the I/O thread must not start
    }
    QVERIFY2(t.elapsed() < 1000, "unused facade must construct/destruct instantly");
}

void TestFacadeThreading::getSet_marshalsToBackendThread()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });

    QVERIFY(serial.set_add_ssm_header(true));      // first call: starts the I/O thread
    QVERIFY(fake != nullptr);
    QVERIFY2(fake->thread() != QThread::currentThread(),
             "backend must live on the I/O thread, not the caller's");
    QCOMPARE(serial.get_add_ssm_header(), true);

    serial.set_serial_port_baudrate("10400");
    QCOMPARE(serial.get_serial_port_baudrate(), QString("10400"));
}

void TestFacadeThreading::scriptedRead_returnsThroughFacade()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });

    serial.set_add_ssm_header(false);   // force backend creation
    fake->scriptedResponse = QByteArray("\x80\xf0\x10\x02\xaa\xbb\x11", 7);

    QCOMPARE(serial.read_serial_data(100), fake->scriptedResponse);
    const QStringList log = fake->takeCallLog();
    QCOMPARE(log.first(), QString("read:begin:t=100"));
    QCOMPARE(log.last(), QString("read:end"));
}

int run_test_facade_threading(int argc, char **argv)
{
    TestFacadeThreading t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_facade_threading.moc"
```

Wire in: add `test_facade_threading.cpp` + `remote/facade sources` to `tests/serial_backend_tests.pro`:

```qmake
SOURCES += test_facade_threading.cpp \
           ../serial_port/serial_port_actions.cpp \
           ../serial_port/serial_backend_host.cpp
HEADERS += fake_backend.h \
           test_facade_threading.h \
           ../serial_port/serial_port_actions.h \
           ../serial_port/serial_backend_host.h
```

Also include `test_facade_threading.h` + `run_test_facade_threading` in `tests/serial_backend_main.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `cd tests && qmake6 serial_backend_tests.pro && make`
Expected: compile FAILURE — `serial_backend_host.h` missing; `SerialPortActions` has no factory parameter.

- [ ] **Step 3: Create `serial_port/serial_backend_host.h`**

```cpp
#ifndef SERIAL_BACKEND_HOST_H
#define SERIAL_BACKEND_HOST_H

#include <QObject>
#include <QThread>
#include <functional>

class SerialBackend;

// Owns the SerialIoThread and the backend living on it. The spec's
// "SerialIoThread" component is this class's m_thread. All backend calls are
// marshaled by SerialPortActions onto context(); the backend is constructed
// and destructed on the thread, so QSerialPort/QSocketNotifier affinity is
// correct by construction.
class SerialBackendHost
{
public:
    SerialBackendHost();
    ~SerialBackendHost();   // deletes the backend on the I/O thread, then joins

    // Runs `factory` on the I/O thread (blocking) and returns the backend.
    SerialBackend *createBackend(const std::function<SerialBackend *()> &factory);

    QObject *context() const { return m_context; }
    QThread *ioThread() { return &m_thread; }

private:
    QThread m_thread;
    QObject *m_context = nullptr;   // affinity: m_thread; invokeMethod target
    SerialBackend *m_backend = nullptr;
};

#endif // SERIAL_BACKEND_HOST_H
```

- [ ] **Step 4: Create `serial_port/serial_backend_host.cpp`**

```cpp
#include "serial_backend_host.h"

#include <QCoreApplication>
#include "serial_backend.h"

SerialBackendHost::SerialBackendHost()
{
    m_thread.setObjectName("SerialIoThread");
    m_context = new QObject();
    m_context->moveToThread(&m_thread);
    m_thread.start();
}

SerialBackendHost::~SerialBackendHost()
{
    Q_ASSERT(QThread::currentThread() != &m_thread);
    if (m_backend)
    {
        SerialBackend *b = m_backend;
        m_backend = nullptr;
        QMetaObject::invokeMethod(m_context, [b] { delete b; },
                                  Qt::BlockingQueuedConnection);
    }
    m_thread.quit();
    m_thread.wait();
    delete m_context;   // safe: its thread has finished
}

SerialBackend *SerialBackendHost::createBackend(const std::function<SerialBackend *()> &factory)
{
    QMetaObject::invokeMethod(m_context, [this, &factory] { m_backend = factory(); },
                              Qt::BlockingQueuedConnection);
    return m_backend;
}
```

- [ ] **Step 5: Rewrite `serial_port/serial_port_actions.h`**

```cpp
#ifndef SERIAL_PORT_ACTIONS_H
#define SERIAL_PORT_ACTIONS_H

#include <QCoreApplication>
#include <QMutex>
#include <QObject>
#include <QSemaphore>
#include <QThread>
#include <QtRemoteObjects/qremoteobjectnode.h>
#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>

// Kept for consumers that relied on this header's transitive includes
// (QSerialPort types, WebSocketIoDevice). Candidates for removal in spec 1b.
#include "serial_port_actions_direct.h"
#include "websocketiodevice.h"

#include "serial_backend.h"

class SerialBackendHost;

// Thin marshaling facade over a SerialBackend hosted on the SerialIoThread
// (see docs/superpowers/specs/2026-07-02-serial-backend-decoupling-design.md).
// The public surface is unchanged from the pre-refactor class; every method
// forwards to the backend on the I/O thread and blocks the caller until the
// result is ready.
class SerialPortActions : public QObject
{
    Q_OBJECT

signals:
    void stateChanged(QRemoteObjectReplica::State state, QRemoteObjectReplica::State oldState);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

public:
    explicit SerialPortActions(QString peerAddress = "",
                               QString password = "",
                               QWebSocket *web_socket = nullptr,
                               QObject *parent = nullptr,
                               std::function<SerialBackend *()> backendFactoryForTests = {});
    ~SerialPortActions();

    bool isDirectConnection(void);

    // ... the entire pre-refactor public method list, verbatim ...
    // (declarations unchanged: get_/set_ pairs, I/O ops, is_comm_busy group,
    //  read_vbatt, parse_message_to_hex)

public slots:
    void waitForSource(void);

private:
    void ensureBackendStarted();
    void waitForDone(const std::shared_ptr<QSemaphore> &done);

    // Marshal `fn` onto the I/O thread and block until it completes. `fn`
    // runs with m_backend valid and is the ONLY code that touches it.
    template <typename Fn>
    auto runOnBackend(Fn fn)
    {
        using Ret = std::invoke_result_t<Fn &>;
        ensureBackendStarted();
        if (QThread::currentThread() == m_ioThread)
            return fn();   // already on the I/O thread (backend-side callback)
        auto done = std::make_shared<QSemaphore>();
        if constexpr (std::is_void_v<Ret>)
        {
            QMetaObject::invokeMethod(m_ioContext, [fn, done]() mutable
                                      { fn(); done->release(); },
                                      Qt::QueuedConnection);
            waitForDone(done);
        }
        else
        {
            Ret result{};
            QMetaObject::invokeMethod(m_ioContext, [fn, done, &result]() mutable
                                      { result = fn(); done->release(); },
                                      Qt::QueuedConnection);
            waitForDone(done);
            return result;
        }
    }

    QString peerAddress;
    QString password;
    QWebSocket *externalSocket = nullptr;
    std::function<SerialBackend *()> backendFactory;

    QMutex startMutex;
    SerialBackendHost *m_host = nullptr;
    SerialBackend *m_backend = nullptr;
    QObject *m_ioContext = nullptr;   // == m_host->context(), cached
    QThread *m_ioThread = nullptr;    // == m_host->ioThread(), cached

    QAtomicInteger<bool> is_read_vbatt = false;
    QAtomicInteger<bool> is_comm_busy = false;
    std::atomic<unsigned long> vBatt{0};
};

#endif // SERIAL_PORT_ACTIONS_H
```

The elided public method list is copied unchanged from the pre-rewrite header (lines 32–169 of the old file), *minus* `websocket_connected` (moved to `RemoteSerialBackend`) and minus the private remote plumbing.

- [ ] **Step 6: Rewrite `serial_port/serial_port_actions.cpp`**

Core plumbing:

```cpp
#include "serial_port_actions.h"

#include "remote_serial_backend.h"
#include "serial_backend_host.h"

SerialPortActions::SerialPortActions(QString peerAddress, QString password,
                                     QWebSocket *web_socket, QObject *parent,
                                     std::function<SerialBackend *()> backendFactoryForTests)
    : QObject{parent}
    , peerAddress(peerAddress)
    , password(password)
    , externalSocket(web_socket)
    , backendFactory(std::move(backendFactoryForTests))
{
    if (!backendFactory)
    {
        backendFactory = [this]() -> SerialBackend * {
            if (isDirectConnection())
                return new SerialPortActionsDirect();
            return new RemoteSerialBackend(this->peerAddress, this->password,
                                           this->externalSocket);
        };
    }
}

SerialPortActions::~SerialPortActions()
{
    QMutexLocker locker(&startMutex);
    delete m_host;   // deletes the backend on the I/O thread, joins the thread
    m_host = nullptr;
    m_backend = nullptr;
}

bool SerialPortActions::isDirectConnection(void)
{
    return (peerAddress == "");
}

void SerialPortActions::ensureBackendStarted()
{
    QMutexLocker locker(&startMutex);
    if (m_backend)
        return;
    m_host = new SerialBackendHost();
    m_ioContext = m_host->context();
    m_ioThread = m_host->ioThread();
    m_backend = m_host->createBackend(backendFactory);

    QObject *b = m_backend->qobject();
    connect(b, SIGNAL(LOG_E(QString,bool,bool)), this, SIGNAL(LOG_E(QString,bool,bool)));
    connect(b, SIGNAL(LOG_W(QString,bool,bool)), this, SIGNAL(LOG_W(QString,bool,bool)));
    connect(b, SIGNAL(LOG_I(QString,bool,bool)), this, SIGNAL(LOG_I(QString,bool,bool)));
    connect(b, SIGNAL(LOG_D(QString,bool,bool)), this, SIGNAL(LOG_D(QString,bool,bool)));
    if (b->metaObject()->indexOfSignal(
            QMetaObject::normalizedSignature(
                "stateChanged(QRemoteObjectReplica::State,QRemoteObjectReplica::State)")) >= 0)
        connect(b, SIGNAL(stateChanged(QRemoteObjectReplica::State,QRemoteObjectReplica::State)),
                this, SIGNAL(stateChanged(QRemoteObjectReplica::State,QRemoteObjectReplica::State)));
}

void SerialPortActions::waitForDone(const std::shared_ptr<QSemaphore> &done)
{
    if (qApp && QThread::currentThread() == qApp->thread())
    {
        // COMPAT SHIM — deleted by spec 1b (flash-op worker migration).
        // Flash modules still run on the GUI thread; pumping here keeps the
        // progress UI alive during multi-minute operations, exactly like the
        // old in-backend pumps did. Reentrant facade calls dispatched by this
        // pump queue behind the in-flight one on the I/O thread instead of
        // interleaving on the wire.
        while (!done->tryAcquire())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
    else
    {
        done->acquire();
    }
}

void SerialPortActions::waitForSource(void)
{
    runOnBackend([this] { m_backend->waitForSource(); });
}
```

Non-trivial method bodies (busy-flag/vbatt logic preserved exactly):

```cpp
QByteArray SerialPortActions::read_serial_data(uint16_t timeout)
{
    QByteArray response = runOnBackend([this, timeout] {
        QByteArray r = m_backend->read_serial_data(timeout);
        if (get_read_vbatt())
        {
            vBatt.store(m_backend->read_vbatt());
            set_read_vbatt(false);
        }
        return r;
    });
    emit LOG_D("Response: " + parse_message_to_hex(response.mid(0, 20)), true, true);
    set_comm_busy(false);
    return response;
}

unsigned long SerialPortActions::read_vbatt()
{
    if (!get_is_comm_busy() && !get_read_vbatt())
        vBatt.store(runOnBackend([this] { return m_backend->read_vbatt(); }));
    else
        set_read_vbatt(true);
    return vBatt.load();
}

bool SerialPortActions::reset_connection()
{
    runOnBackend([this] { m_backend->reset_connection(); });
    return true;
}
```

Every remaining method follows one mechanical pattern — same name, same return, forwarding through `runOnBackend`:

```cpp
bool SerialPortActions::get_add_ssm_header(void)
{
    return runOnBackend([this] { return m_backend->get_add_ssm_header(); });
}
bool SerialPortActions::set_add_ssm_header(bool value)
{
    return runOnBackend([this, value] { return m_backend->set_add_ssm_header(value); });
}
```

Apply it to: all 44 get/set pairs; `is_serial_port_open`, `set_kline_timings`, `check_serial_ports`, `open_serial_port`, `get_is_tx_done` (plain forwards); and the busy-flag-wrapped operations, keeping each one's `set_comm_busy` calls **exactly where the old code had them** around the single forward — `change_port_speed`, `set_lec_lines`, `pulse_lec_1_line`, `pulse_lec_2_line`, `clear_rx_buffer`, `clear_tx_buffer`, `send_periodic_j2534_data`, `stop_periodic_j2534_data` (set true before / set false after); `set_j2534_ioctl`, `five_baud_init`, `fast_init`, `write_serial_data`, `write_serial_data_echo_check` (set true before, **no** clear — that asymmetry is load-bearing today, `read_serial_data` clears it); `read_serial_obd_data` (clear after, plus its LOG_D line). `get_is_comm_busy`/`set_comm_busy`/`get_read_vbatt`/`set_read_vbatt`/`parse_message_to_hex` keep their current facade-local bodies (no marshal). Delete the old `delay()` method — nothing references it afterwards.

- [ ] **Step 7: Wire the new files into the remaining .pro files**

- `FastECU.pro`: add `serial_port/serial_backend_host.cpp` to SOURCES, `serial_port/serial_backend_host.h` to HEADERS.
- `tests/tests.pro`: change `CONFIG += c++11` → `c++17`; add to SOURCES: `../serial_port/remote_serial_backend.cpp`, `../serial_port/serial_backend_host.cpp`; add to HEADERS: `../serial_port/serial_backend.h`, `../serial_port/remote_serial_backend.h`, `../serial_port/serial_backend_host.h`.
- `tests/mut_dma_integration_tests.pro`: same additions as tests.pro (it links `serial_port_actions.cpp` — check its SOURCES/HEADERS and mirror).

- [ ] **Step 8: Run everything**

Run: `cd tests && qmake6 serial_backend_tests.pro && make && ./serial_backend_tests`
Expected: 10 tests PASS (3 facade + 6 direct + 1 remote smoke).
Run: `cd tests && qmake6 tests.pro && make && ./mut_dma_tests`
Expected: all existing suites PASS.
Run: `cd tests && qmake6 serial_crash_tests.pro && make && ./serial_crash_tests`
Expected: all PASS.
Run: `qmake6 FastECU.pro && make -j8`
Expected: clean app build. If consumer TUs fail on missing includes (transitive-include fallout), add the specific missing `#include` to the *consumer* cpp — expected candidates: none, since the facade header keeps `serial_port_actions_direct.h`/`websocketiodevice.h` includes.

- [ ] **Step 9: Verify the single-pump invariant**

Run: `grep -rn "processEvents" serial_port/`
Expected: exactly one hit, in `serial_port_actions.cpp`, on the `COMPAT SHIM` line inside `waitForDone`.

- [ ] **Step 10: Commit**

```bash
git add serial_port/ FastECU.pro tests/
git commit -m "feat: host serial backends on a dedicated I/O thread behind a marshaling facade"
```

---

### Task 6: Thread-safety test suite

The spec's thread-safety level: worker-thread callers with zero affinity warnings, serialization of concurrent callers, and GUI-shim event-loop liveness.

**Files:**
- Modify: `tests/test_facade_threading.cpp` (append four tests + warning capture helper)

**Interfaces:**
- Consumes: `SerialPortActions` test factory + `FakeBackend` (Task 5).
- Produces: nothing new.

- [ ] **Step 1: Write the four failing/green tests**

Append to `tests/test_facade_threading.cpp` (declare the new slots in the class; add includes `<QTimer>`, `<QElapsedTimer>`, `<vector>`):

```cpp
// ---- affinity-warning capture ------------------------------------------
static QStringList g_threadWarnings;
static QtMessageHandler g_prevHandler = nullptr;

static void warningCapture(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (type == QtWarningMsg &&
        (msg.contains("another thread") || msg.contains("different thread")))
        g_threadWarnings.append(msg);
    if (g_prevHandler)
        g_prevHandler(type, ctx, msg);
}

void TestFacadeThreading::workerThreadCaller_noAffinityWarnings()
{
    // The exact LoggingWorker scenario from the bench checklist: a non-GUI
    // thread drives the facade. Data must arrive and Qt must emit no
    // cross-thread affinity warnings.
    g_threadWarnings.clear();
    g_prevHandler = qInstallMessageHandler(warningCapture);

    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });

    QByteArray got;
    std::thread worker([&] {
        serial.set_add_ssm_header(true);
        fake->scriptedResponse = QByteArray("\x80\xf0\x10\x01\x55\x66", 6);
        got = serial.read_serial_data(100);
    });
    worker.join();

    qInstallMessageHandler(g_prevHandler);
    QCOMPARE(got, QByteArray("\x80\xf0\x10\x01\x55\x66", 6));
    QVERIFY2(g_threadWarnings.isEmpty(),
             qPrintable("affinity warnings: " + g_threadWarnings.join(" | ")));
}

void TestFacadeThreading::concurrentCallers_serializeWithoutInterleaving()
{
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);   // create backend
    fake->readDelayMs = 20;             // widen the interleave window
    fake->takeCallLog();

    auto hammer = [&serial](int n) {
        for (int i = 0; i < n; ++i)
        {
            serial.write_serial_data(QByteArray(1, char(i)));
            serial.read_serial_data(10);
        }
    };
    std::thread a(hammer, 5), b(hammer, 5);
    a.join();
    b.join();

    // Every begin must be immediately followed by its matching end: an
    // interleaved wire access would show two consecutive "begin" entries.
    const QStringList log = fake->takeCallLog();
    QCOMPARE(log.size(), 40);   // 2 threads * 5 iterations * 2 ops * (begin+end)
    for (int i = 0; i < log.size(); i += 2)
    {
        QVERIFY2(log.at(i).contains(":begin"), qPrintable(log.at(i)));
        QVERIFY2(log.at(i + 1).contains(":end"), qPrintable(log.at(i + 1)));
        QCOMPARE(log.at(i).section(':', 0, 0), log.at(i + 1).section(':', 0, 0));
    }
}

void TestFacadeThreading::guiThreadCaller_compatShim_keepsEventLoopAlive()
{
    // Main thread == qApp->thread(): the compat shim path. A timer must keep
    // firing while the facade blocks in a 300ms backend call — this is what
    // keeps flash-module progress UI painting until spec 1b.
    FakeBackend *fake = nullptr;
    SerialPortActions serial("", "", nullptr, nullptr,
                             [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
    serial.set_add_ssm_header(false);
    fake->readDelayMs = 300;

    int ticks = 0;
    QTimer timer;
    timer.setInterval(10);
    QObject::connect(&timer, &QTimer::timeout, [&ticks] { ticks++; });
    timer.start();

    serial.read_serial_data(10);   // blocks ~300ms on the fake backend
    timer.stop();

    QVERIFY2(ticks >= 5, qPrintable(QString("timer only ticked %1 times").arg(ticks)));
}

void TestFacadeThreading::destroyAfterUse_joinsIoThread()
{
    QPointer<QThread> ioThread;
    {
        FakeBackend *fake = nullptr;
        SerialPortActions serial("", "", nullptr, nullptr,
                                 [&fake]() -> SerialBackend * { fake = new FakeBackend(); return fake; });
        serial.set_add_ssm_header(true);
        ioThread = fake->thread();
        QVERIFY(ioThread && ioThread->isRunning());
    }
    QVERIFY2(!ioThread || !ioThread->isRunning(),
             "facade destruction must stop and join the I/O thread");
}
```

- [ ] **Step 2: Run the suite**

Run: `cd tests && qmake6 serial_backend_tests.pro && make && ./serial_backend_tests`
Expected: all 14 tests PASS. (These are green-on-arrival if Task 5 is correct — their value is pinning the guarantees; if any fails, Task 5 has a real bug: fix it there, don't weaken the test.)

- [ ] **Step 3: Commit**

```bash
git add tests/test_facade_threading.cpp
git commit -m "test: pin thread-safety guarantees of the serial facade (affinity, serialization, shim liveness)"
```

---

### Task 7: Plain-serial PTY end-to-end through the facade

The bench-checklist item 1 analog, headless: a worker thread (the LoggingWorker scenario) drives the facade; the real direct backend opens a PTY on the I/O thread; a responder thread plays the ECU. Proves `waitForReadyRead`-based delivery works with correct affinity end-to-end.

**Files:**
- Create: `tests/test_pty_e2e.h`, `tests/test_pty_e2e.cpp`
- Modify: `tests/serial_backend_main.cpp`, `tests/serial_backend_tests.pro`

**Interfaces:**
- Consumes: `SerialPortActions` (default factory — real backends).
- Produces: nothing new.

- [ ] **Step 1: Write the test**

`tests/test_pty_e2e.h`:

```cpp
#ifndef TEST_PTY_E2E_H
#define TEST_PTY_E2E_H
int run_test_pty_e2e(int argc, char **argv);
#endif
```

`tests/test_pty_e2e.cpp`:

```cpp
#include "test_pty_e2e.h"

#include <QtTest>
#include <atomic>
#include <thread>

#include <util.h>      // openpty (macOS)
#include <unistd.h>

#include "serial_port_actions.h"

// End-to-end over a pseudo-terminal, all through the production stack:
// facade -> I/O thread -> real SerialPortActionsDirect -> QSerialPort(pty).
// The caller is a WORKER thread (the LoggingWorker scenario, bench checklist
// item 1); the "ECU" is a responder thread on the pty master.
class TestPtyE2e : public QObject
{
    Q_OBJECT
private slots:
    void workerThread_writeRead_overPty_deliversFramedMessage();
};

void TestPtyE2e::workerThread_writeRead_overPty_deliversFramedMessage()
{
    int master = -1, slave = -1;
    char name[256] = {0};
    QVERIFY2(openpty(&master, &slave, name, nullptr, nullptr) == 0, "openpty failed");

    // ECU responder: on receiving anything, replies with one SSM-framed
    // message (header 80 f0 10, len 02, payload aa bb, checksum byte).
    std::atomic<bool> stop{false};
    QByteArray received;
    std::thread responder([&] {
        const char reply[] = "\x80\xf0\x10\x02\xaa\xbb\xcc";
        char buf[64];
        bool replied = false;
        while (!stop.load())
        {
            ssize_t n = ::read(master, buf, sizeof(buf));   // pty master: blocking
            if (n > 0)
            {
                received.append(buf, int(n));
                if (!replied) { ::write(master, reply, 7); replied = true; }
            }
            else if (n < 0)
                break;
        }
    });

    SerialPortActions serial;   // default factory: real direct backend
    QByteArray response;
    QString opened;
    std::thread worker([&] {
        serial.set_serial_port_prefix_linux("");
        serial.set_serial_port_list(QStringList() << QString::fromLocal8Bit(name));
        opened = serial.open_serial_port();
        serial.write_serial_data(QByteArray("\x01\x02\x03", 3));
        response = serial.read_serial_data(2000);
    });
    worker.join();
    stop.store(true);
    ::close(master);     // unblocks the responder's read
    responder.join();

    QCOMPARE(opened, QString::fromLocal8Bit(name));
    QCOMPARE(received.left(3), QByteArray("\x01\x02\x03", 3));
    QCOMPARE(response, QByteArray("\x80\xf0\x10\x02\xaa\xbb\xcc", 7));
}

int run_test_pty_e2e(int argc, char **argv)
{
    TestPtyE2e t;
    return QTest::qExec(&t, argc, argv);
}

#include "test_pty_e2e.moc"
```

Wire in: `test_pty_e2e.cpp`/`test_pty_e2e.h` into `tests/serial_backend_tests.pro` (unix-only: put both inside the existing `unix { }` block — openpty is POSIX); include + `status |= run_test_pty_e2e(argc, argv);` in `tests/serial_backend_main.cpp` guarded by `#ifdef Q_OS_UNIX`.

- [ ] **Step 2: Run it**

Run: `cd tests && qmake6 serial_backend_tests.pro && make && ./serial_backend_tests`
Expected: all 15 tests PASS. If `workerThread_writeRead_overPty_deliversFramedMessage` hangs or times out, that is a real Task 5 defect (queued call not reaching the I/O thread, or `waitForReadyRead` not delivering on the pty) — debug there.

- [ ] **Step 3: Commit**

```bash
git add tests/test_pty_e2e.h tests/test_pty_e2e.cpp tests/serial_backend_main.cpp tests/serial_backend_tests.pro
git commit -m "test: plain-serial pty end-to-end through the facade and I/O thread from a worker thread"
```

---

### Task 8: Full regression pass + documentation

**Files:**
- Modify: `docs/logging-engine-tech-debt.md`, `docs/logging-engine-bench-checklist.md`
- No code changes expected; fix anything the full pass surfaces.

**Interfaces:** none.

- [ ] **Step 1: Full build + all suites**

Run each; expected all PASS / clean:

```bash
qmake6 FastECU.pro && make -j8
cd tests && qmake6 tests.pro && make && ./mut_dma_tests
qmake6 serial_crash_tests.pro && make && ./serial_crash_tests
qmake6 serial_backend_tests.pro && make && ./serial_backend_tests
qmake6 mut_dma_integration_tests.pro && make && ./mut_dma_integration_tests
```

(Adjust the integration binary name to the TARGET in its .pro if it differs.)

- [ ] **Step 2: Verify the global invariants**

```bash
grep -rn "processEvents" serial_port/        # exactly 1 hit: the COMPAT SHIM
git diff master --stat -- mainwindow.cpp mainwindow.h ecu_operations.cpp dtc_operations.cpp modules/ logging/ protocol/   # empty: no consumer changed
```

- [ ] **Step 3: Update documentation**

In `docs/logging-engine-tech-debt.md`, under "Structural / build-system debt", replace the `SerialPortActions` blocking-I/O paragraph's final sentence with a note that the thread-safe I/O path landed (this refactor, spec `2026-07-02-serial-backend-decoupling-design.md`): backends now live on `SerialIoThread`, `processEvents()` remains only in the facade's GUI compat shim pending spec 1b.

In `docs/logging-engine-bench-checklist.md`, item 1 (direct-serial under the new threading model): append a note that the architectural fix landed — `QSerialPort` is now created on the I/O thread and the pump is gone — and that the item should be re-verified on hardware against the new stack (the headless analog is `tests/test_pty_e2e.cpp`).

- [ ] **Step 4: Commit**

```bash
git add docs/logging-engine-tech-debt.md docs/logging-engine-bench-checklist.md
git commit -m "docs: record serial backend decoupling landing; flag bench re-verification"
```

- [ ] **Step 5: Bench gate (manual, post-merge)**

Not automatable: re-run `docs/logging-engine-bench-checklist.md` item 1 on real hardware (OpenPort 2.0 + plain-serial adapter), plus a manual remote-mode smoke test (spec risk: remote path has no automated call tests). Flag these to the user as the release gate.
