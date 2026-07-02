# FastECU Logging Engine Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace FastECU's three GUI-thread, QTimer-driven logging implementations (SSM, MUT/DMA, Cdbg) with a single thread-based, protocol-extensible logging engine, per `docs/superpowers/specs/2026-07-02-logging-engine-refactor-design.md`.

**Architecture:** A `LoggingProtocol` interface (`start()`/`poll(timeoutMs)`/`stop()`) is implemented once per protocol (SSM, MUT/DMA, Cdbg). A `LoggingWorker` (`QThread` subclass) runs whichever protocol is active in a tight `while` loop with no imposed cadence, emitting queued signals back to the GUI thread. A `LoggingEngine` (GUI-thread `QObject`) owns a small protocol registry and manages the current session's worker lifecycle.

**Tech Stack:** C++17, Qt 6 (`QThread`, `QAtomicInteger`, `QtTest`/`QSignalSpy` for tests), qmake (`.pro` files).

## Global Constraints

- No real hardware or emulator in any test added by this plan — all new tests use scripted/mocked transports (`ScriptedKlineTransport`, `ScriptedCanTransport`, and a new `ScriptedSsmTransport`), matching `tests/test_driver.cpp`'s existing pattern.
- All three existing protocols (SSM, MUT/DMA, Cdbg) are ported in this effort — none is left on the old GUI-thread path.
- Per-session configuration is fixed for the session's lifetime; changing it means stop-then-start (no live reconfiguration).
- `SerialPortActions`'s internal blocking-I/O implementation is not modified — only which thread calls it changes.
- Every new/modified `.cpp`/`.h` file must be added to `FastECU.pro` (app) or `tests/tests.pro` (tests) as appropriate, or it will not build.

---

## Task 1: `LoggingProtocol` interface + `LoggingWorker` + worker unit tests

**Files:**
- Create: `logging/logging_protocol.h`
- Create: `logging/logging_worker.h`
- Create: `logging/logging_worker.cpp`
- Create: `tests/scripted_logging_protocol.h`
- Create: `tests/test_logging_worker.h`
- Create: `tests/test_logging_worker.cpp`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`
- Modify: `FastECU.pro`

**Interfaces:**
- Produces: `LogSample{int logValueIndex; QString displayValue;}`, `PollResult{Status status; QVector<LogSample> samples; QString errorMessage;}` with `Status` = `Ok`/`NoResponse`/`TransportError`, `LoggingStatus` (`Running`/`CarNotResponding`), `SessionEndReason` (`StoppedByUser`/`HandshakeFailed`/`AdapterDisconnected`), abstract class `LoggingProtocol` with `virtual bool start(QString *errorOut) = 0; virtual PollResult poll(int timeoutMs) = 0; virtual void stop() = 0;`. `LoggingWorker : public QThread` with constructor `LoggingWorker(LoggingProtocol *protocol, int pollTimeoutMs, int carSilenceMissThreshold, int reconnectAttemptThreshold, int reconnectRetryPeriod, QObject *parent = nullptr)`, method `void requestStop()`, signals `valuesUpdated(QVector<LogSample>)`, `statusChanged(LoggingStatus)`, `sessionEnded(SessionEndReason, QString)`, `LOG_E/W/I/D(QString, bool, bool)`.
- Consumes: nothing from earlier tasks (this is the first task).

- [ ] **Step 1: Write `logging/logging_protocol.h`**

```cpp
#pragma once
#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVector>

// One converted, display-ready value for a single FileActions::LogValuesStructure
// entry (identified by its index into that struct's parallel QStringLists).
struct LogSample {
    int logValueIndex;
    QString displayValue;
};

// Outcome of one LoggingProtocol::poll() call.
struct PollResult {
    enum class Status { Ok, NoResponse, TransportError };
    Status status = Status::NoResponse;
    QVector<LogSample> samples;   // valid when status == Ok; already expression-converted
    QString errorMessage;         // valid when status == TransportError
};

// Recoverable connectivity state of an active logging session.
enum class LoggingStatus { Running, CarNotResponding };

// Why a logging session's thread ended.
enum class SessionEndReason { StoppedByUser, HandshakeFailed, AdapterDisconnected };

// One implementation per wire protocol (SSM, MUT/DMA, Cdbg, ...). Polling-type
// protocols send a request and block for the response inside poll(); setup-and-listen
// protocols block until the next pushed frame arrives. Either way, poll() is called
// back-to-back by LoggingWorker with no imposed delay between calls.
class LoggingProtocol {
public:
    virtual ~LoggingProtocol() = default;
    virtual bool start(QString *errorOut) = 0;
    virtual PollResult poll(int timeoutMs) = 0;
    virtual void stop() = 0;
};

Q_DECLARE_METATYPE(LogSample)
Q_DECLARE_METATYPE(QVector<LogSample>)
Q_DECLARE_METATYPE(LoggingStatus)
Q_DECLARE_METATYPE(SessionEndReason)
```

- [ ] **Step 2: Write `logging/logging_worker.h`**

```cpp
#pragma once
#include <QAtomicInteger>
#include <QThread>
#include "logging/logging_protocol.h"

// Drives one LoggingProtocol in a dedicated thread. Does not use a QTimer or the
// thread's own event loop for pacing: run() is a tight loop where each protocol's
// own blocking poll() call provides the pacing, so a polling-type protocol sends
// its next request immediately upon receiving the previous response, and a
// setup-and-listen protocol processes each frame as soon as it arrives.
//
// requestStop() is safe to call from any thread without the worker running an
// event loop, because it only sets an atomic flag that run()'s loop checks between
// poll() calls (each bounded by pollTimeoutMs).
class LoggingWorker : public QThread {
    Q_OBJECT
public:
    LoggingWorker(LoggingProtocol *protocol, int pollTimeoutMs,
                  int carSilenceMissThreshold, int reconnectAttemptThreshold,
                  int reconnectRetryPeriod, QObject *parent = nullptr);
    ~LoggingWorker() override;

    void requestStop();

signals:
    void valuesUpdated(QVector<LogSample> samples);
    void statusChanged(LoggingStatus status);
    void sessionEnded(SessionEndReason reason, QString message);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

protected:
    void run() override;

private:
    LoggingProtocol *m_protocol; // not owned
    int m_pollTimeoutMs;
    int m_carSilenceMissThreshold;
    int m_reconnectAttemptThreshold;
    int m_reconnectRetryPeriod;
    QAtomicInteger<bool> m_stopRequested { false };
};
```

- [ ] **Step 3: Write `logging/logging_worker.cpp`**

```cpp
#include "logging/logging_worker.h"

LoggingWorker::LoggingWorker(LoggingProtocol *protocol, int pollTimeoutMs,
                              int carSilenceMissThreshold, int reconnectAttemptThreshold,
                              int reconnectRetryPeriod, QObject *parent)
    : QThread(parent)
    , m_protocol(protocol)
    , m_pollTimeoutMs(pollTimeoutMs)
    , m_carSilenceMissThreshold(carSilenceMissThreshold)
    , m_reconnectAttemptThreshold(reconnectAttemptThreshold)
    , m_reconnectRetryPeriod(reconnectRetryPeriod)
{
}

LoggingWorker::~LoggingWorker()
{
    requestStop();
    wait();
}

void LoggingWorker::requestStop()
{
    m_stopRequested.storeRelaxed(true);
}

void LoggingWorker::run()
{
    QString err;
    if (!m_protocol->start(&err)) {
        emit LOG_E("Logging session failed to start: " + err, true, true);
        emit sessionEnded(SessionEndReason::HandshakeFailed, err);
        return;
    }

    emit statusChanged(LoggingStatus::Running);
    LoggingStatus lastStatus = LoggingStatus::Running;
    int consecutiveMisses = 0;

    while (!m_stopRequested.loadRelaxed()) {
        PollResult r = m_protocol->poll(m_pollTimeoutMs);
        switch (r.status) {
        case PollResult::Status::Ok:
            consecutiveMisses = 0;
            if (lastStatus != LoggingStatus::Running) {
                lastStatus = LoggingStatus::Running;
                emit statusChanged(LoggingStatus::Running);
            }
            emit valuesUpdated(r.samples);
            break;

        case PollResult::Status::NoResponse:
            ++consecutiveMisses;
            if (consecutiveMisses == m_carSilenceMissThreshold) {
                lastStatus = LoggingStatus::CarNotResponding;
                emit LOG_W("Car not responding", true, true);
                emit statusChanged(LoggingStatus::CarNotResponding);
            }
            if (consecutiveMisses >= m_reconnectAttemptThreshold
                && (consecutiveMisses - m_reconnectAttemptThreshold) % m_reconnectRetryPeriod == 0) {
                QString reErr;
                if (m_protocol->start(&reErr)) {
                    consecutiveMisses = 0;
                    lastStatus = LoggingStatus::Running;
                    emit LOG_I("Car logging resumed", true, true);
                    emit statusChanged(LoggingStatus::Running);
                }
            }
            break;

        case PollResult::Status::TransportError:
            m_protocol->stop();
            emit LOG_E("Adapter disconnected: " + r.errorMessage, true, true);
            emit sessionEnded(SessionEndReason::AdapterDisconnected, r.errorMessage);
            return;
        }
    }

    m_protocol->stop();
    emit sessionEnded(SessionEndReason::StoppedByUser, QString());
}
```

- [ ] **Step 4: Write `tests/scripted_logging_protocol.h`**

```cpp
#pragma once
#include "logging/logging_protocol.h"
#include <QList>

// Test double: scripts a sequence of start()/poll() outcomes for LoggingWorker
// and LoggingEngine tests. Once a queue is exhausted, start() fails with a
// diagnostic message and poll() returns Ok with no samples (a safe default that
// never spuriously degrades a test's status assertions).
class ScriptedLoggingProtocol : public LoggingProtocol {
public:
    void queueStartResult(bool ok, const QString &error = QString()) {
        startResultsOk_.append(ok);
        startResultsError_.append(error);
    }
    void queuePollResult(const PollResult &result) { pollResults_.append(result); }

    int startCallCount() const { return startCalls_; }
    bool stopCalled() const { return stopCalled_; }

    bool start(QString *errorOut) override {
        ++startCalls_;
        if (startIdx_ >= startResultsOk_.size()) {
            if (errorOut) *errorOut = "no scripted start result";
            return false;
        }
        bool ok = startResultsOk_.at(startIdx_);
        if (errorOut) *errorOut = startResultsError_.at(startIdx_);
        ++startIdx_;
        return ok;
    }

    PollResult poll(int) override {
        if (pollIdx_ >= pollResults_.size()) {
            PollResult r;
            r.status = PollResult::Status::Ok;
            return r;
        }
        return pollResults_.at(pollIdx_++);
    }

    void stop() override { stopCalled_ = true; }

private:
    QList<bool> startResultsOk_;
    QList<QString> startResultsError_;
    QList<PollResult> pollResults_;
    int startIdx_ = 0;
    int pollIdx_ = 0;
    int startCalls_ = 0;
    bool stopCalled_ = false;
};
```

- [ ] **Step 5: Write `tests/test_logging_worker.h`**

```cpp
#pragma once
int run_test_logging_worker(int argc, char** argv);
```

- [ ] **Step 6: Write the failing tests in `tests/test_logging_worker.cpp`**

```cpp
#include <QtTest>
#include <QSignalSpy>
#include "logging/logging_worker.h"
#include "scripted_logging_protocol.h"
#include "test_logging_worker.h"

class TestLoggingWorker : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qRegisterMetaType<QVector<LogSample>>();
        qRegisterMetaType<LoggingStatus>();
        qRegisterMetaType<SessionEndReason>();
    }

    void normal_run_emits_running_then_values() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(true);
        PollResult ok1;
        ok1.status = PollResult::Status::Ok;
        ok1.samples.append(LogSample{0, "1234"});
        proto.queuePollResult(ok1);

        LoggingWorker worker(&proto, 20, 5, 10, 10);
        QSignalSpy statusSpy(&worker, &LoggingWorker::statusChanged);
        QSignalSpy valuesSpy(&worker, &LoggingWorker::valuesUpdated);

        worker.start();
        QVERIFY(valuesSpy.wait(2000));
        worker.requestStop();
        QVERIFY(worker.wait(2000));

        QVERIFY(statusSpy.size() >= 1);
        QCOMPARE(statusSpy.at(0).at(0).value<LoggingStatus>(), LoggingStatus::Running);
        QVector<LogSample> samples = valuesSpy.at(0).at(0).value<QVector<LogSample>>();
        QCOMPARE(samples.size(), 1);
        QCOMPARE(samples.at(0).logValueIndex, 0);
        QCOMPARE(samples.at(0).displayValue, QString("1234"));
    }

    void handshake_failure_ends_session_without_polling() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(false, "handshake rejected");

        LoggingWorker worker(&proto, 20, 5, 10, 10);
        QSignalSpy endedSpy(&worker, &LoggingWorker::sessionEnded);

        worker.start();
        QVERIFY(worker.wait(2000));

        QCOMPARE(endedSpy.size(), 1);
        QCOMPARE(endedSpy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::HandshakeFailed);
        QCOMPARE(endedSpy.at(0).at(1).toString(), QString("handshake rejected"));
    }

    void car_dropout_then_recovery_flips_status_back() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(true);
        for (int i = 0; i < 5; ++i) {
            PollResult r;
            r.status = PollResult::Status::NoResponse;
            proto.queuePollResult(r);
        }
        PollResult recovered;
        recovered.status = PollResult::Status::Ok;
        recovered.samples.append(LogSample{0, "1"});
        proto.queuePollResult(recovered);

        LoggingWorker worker(&proto, 5, 5, 1000, 1000);
        QSignalSpy statusSpy(&worker, &LoggingWorker::statusChanged);

        worker.start();
        QTRY_COMPARE_WITH_TIMEOUT(statusSpy.size(), 3, 3000);
        worker.requestStop();
        QVERIFY(worker.wait(2000));

        QCOMPARE(statusSpy.at(0).at(0).value<LoggingStatus>(), LoggingStatus::Running);
        QCOMPARE(statusSpy.at(1).at(0).value<LoggingStatus>(), LoggingStatus::CarNotResponding);
        QCOMPARE(statusSpy.at(2).at(0).value<LoggingStatus>(), LoggingStatus::Running);
    }

    void adapter_loss_ends_session_and_thread_exits() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(true);
        PollResult err;
        err.status = PollResult::Status::TransportError;
        err.errorMessage = "port closed";
        proto.queuePollResult(err);

        LoggingWorker worker(&proto, 20, 5, 10, 10);
        QSignalSpy endedSpy(&worker, &LoggingWorker::sessionEnded);

        worker.start();
        QVERIFY(worker.wait(2000));

        QCOMPARE(endedSpy.size(), 1);
        QCOMPARE(endedSpy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::AdapterDisconnected);
        QVERIFY(proto.stopCalled());
    }

    void stop_is_noticed_within_one_poll_cycle() {
        ScriptedLoggingProtocol proto;
        proto.queueStartResult(true);
        for (int i = 0; i < 1000; ++i) {
            PollResult r;
            r.status = PollResult::Status::NoResponse;
            proto.queuePollResult(r);
        }

        LoggingWorker worker(&proto, 10, 100000, 100000, 100000);
        worker.start();
        QTest::qWait(30);
        worker.requestStop();
        QVERIFY(worker.wait(500));
    }
};

int run_test_logging_worker(int argc, char** argv) {
    TestLoggingWorker t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_logging_worker.moc"
```

- [ ] **Step 7: Wire the new files into the build**

In `tests/tests.pro`, add to `SOURCES`:
```
    test_logging_worker.cpp \
    ../logging/logging_worker.cpp \
```
and to `HEADERS`:
```
    test_logging_worker.h \
    scripted_logging_protocol.h \
    ../logging/logging_protocol.h \
    ../logging/logging_worker.h \
```

In `tests/main.cpp`, add before the final `return status;`:
```cpp
    status |= run_test_logging_worker(argc, argv);
```

In `FastECU.pro`, add `logging/logging_worker.cpp` to `SOURCES` and `logging/logging_protocol.h`, `logging/logging_worker.h` to `HEADERS` (match the existing style of the `protocol/*.cpp`/`.h` entries at lines 162-171/248-259).

- [ ] **Step 8: Build and run the new test binary**

Run: `cd tests && qmake tests.pro && make && ./mut_dma_tests`
Expected: all existing suites still pass, plus a new `TestLoggingWorker` section reporting 5 passed, 0 failed.

- [ ] **Step 9: Commit**

```bash
git add logging/logging_protocol.h logging/logging_worker.h logging/logging_worker.cpp \
        tests/scripted_logging_protocol.h tests/test_logging_worker.h tests/test_logging_worker.cpp \
        tests/tests.pro tests/main.cpp FastECU.pro
git commit -m "feat: add LoggingProtocol interface and LoggingWorker thread"
```

---

## Task 2: `LoggingEngine` façade + registry + tests

**Files:**
- Create: `logging/logging_engine.h`
- Create: `logging/logging_engine.cpp`
- Create: `tests/test_logging_engine.h`
- Create: `tests/test_logging_engine.cpp`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`
- Modify: `FastECU.pro`

**Interfaces:**
- Consumes: `LoggingProtocol`, `LogSample`, `PollResult`, `LoggingStatus`, `SessionEndReason` from `logging/logging_protocol.h` (Task 1); `LoggingWorker` from `logging/logging_worker.h` (Task 1); `ScriptedLoggingProtocol` from `tests/scripted_logging_protocol.h` (Task 1).
- Produces: `struct LogSessionConfig { QString protocolId; QString logValueProtocolFilter; };`, `using LoggingProtocolFactory = std::function<std::unique_ptr<LoggingProtocol>(const LogSessionConfig&)>;`, class `LoggingEngine : public QObject` with `void registerProtocol(const QString &protocolId, LoggingProtocolFactory factory, int pollTimeoutMs, int carSilenceMissThreshold, int reconnectAttemptThreshold, int reconnectRetryPeriod)`, `bool start(const LogSessionConfig &config)`, `void stop()`, `bool isRunning() const`, signals `valuesUpdated(QVector<LogSample>)`, `statusChanged(LoggingStatus)`, `sessionEnded(SessionEndReason, QString)`, `LOG_E/W/I/D(QString, bool, bool)`.

- [ ] **Step 1: Write `logging/logging_engine.h`**

```cpp
#pragma once
#include <QMap>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include "logging/logging_protocol.h"

struct LogSessionConfig {
    QString protocolId;              // registry key: "SSM" / "MUT_DMA" / "CDBG"
    QString logValueProtocolFilter;  // string compared against log_value_protocol entries
};

using LoggingProtocolFactory = std::function<std::unique_ptr<LoggingProtocol>(const LogSessionConfig &)>;

// GUI-thread-owned façade over the current logging session's LoggingWorker.
// Adding a future protocol means one call to registerProtocol() -- no changes
// to LoggingEngine or LoggingWorker themselves.
class LoggingEngine : public QObject {
    Q_OBJECT
public:
    explicit LoggingEngine(QObject *parent = nullptr);
    ~LoggingEngine() override;

    void registerProtocol(const QString &protocolId, LoggingProtocolFactory factory,
                           int pollTimeoutMs, int carSilenceMissThreshold,
                           int reconnectAttemptThreshold, int reconnectRetryPeriod);

    // Returns false immediately without starting anything if no factory is
    // registered for config.protocolId, or a session is already running.
    bool start(const LogSessionConfig &config);
    void stop();
    bool isRunning() const;

signals:
    void valuesUpdated(QVector<LogSample> samples);
    void statusChanged(LoggingStatus status);
    void sessionEnded(SessionEndReason reason, QString message);
    void LOG_E(QString message, bool timestamp, bool linefeed);
    void LOG_W(QString message, bool timestamp, bool linefeed);
    void LOG_I(QString message, bool timestamp, bool linefeed);
    void LOG_D(QString message, bool timestamp, bool linefeed);

private slots:
    void handleWorkerSessionEnded(SessionEndReason reason, QString message);

private:
    struct ProtocolRegistration {
        LoggingProtocolFactory factory;
        int pollTimeoutMs;
        int carSilenceMissThreshold;
        int reconnectAttemptThreshold;
        int reconnectRetryPeriod;
    };

    QMap<QString, ProtocolRegistration> m_registrations;
    std::unique_ptr<LoggingProtocol> m_activeProtocol;
    LoggingWorker *m_activeWorker = nullptr;
};
```

- [ ] **Step 2: Write `logging/logging_engine.cpp`**

```cpp
#include "logging/logging_engine.h"
#include "logging/logging_worker.h"

LoggingEngine::LoggingEngine(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<QVector<LogSample>>();
    qRegisterMetaType<LoggingStatus>();
    qRegisterMetaType<SessionEndReason>();
}

LoggingEngine::~LoggingEngine()
{
    stop();
}

void LoggingEngine::registerProtocol(const QString &protocolId, LoggingProtocolFactory factory,
                                      int pollTimeoutMs, int carSilenceMissThreshold,
                                      int reconnectAttemptThreshold, int reconnectRetryPeriod)
{
    m_registrations.insert(protocolId, ProtocolRegistration{
        factory, pollTimeoutMs, carSilenceMissThreshold, reconnectAttemptThreshold, reconnectRetryPeriod
    });
}

bool LoggingEngine::isRunning() const
{
    return m_activeWorker != nullptr;
}

bool LoggingEngine::start(const LogSessionConfig &config)
{
    if (isRunning())
        return false;

    auto it = m_registrations.find(config.protocolId);
    if (it == m_registrations.end()) {
        emit LOG_E("No logging protocol registered for '" + config.protocolId + "'", true, true);
        return false;
    }

    m_activeProtocol = it->factory(config);
    m_activeWorker = new LoggingWorker(m_activeProtocol.get(), it->pollTimeoutMs,
                                        it->carSilenceMissThreshold, it->reconnectAttemptThreshold,
                                        it->reconnectRetryPeriod, this);

    connect(m_activeWorker, &LoggingWorker::valuesUpdated, this, &LoggingEngine::valuesUpdated);
    connect(m_activeWorker, &LoggingWorker::statusChanged, this, &LoggingEngine::statusChanged);
    connect(m_activeWorker, &LoggingWorker::sessionEnded, this, &LoggingEngine::handleWorkerSessionEnded);
    connect(m_activeWorker, &LoggingWorker::LOG_E, this, &LoggingEngine::LOG_E);
    connect(m_activeWorker, &LoggingWorker::LOG_W, this, &LoggingEngine::LOG_W);
    connect(m_activeWorker, &LoggingWorker::LOG_I, this, &LoggingEngine::LOG_I);
    connect(m_activeWorker, &LoggingWorker::LOG_D, this, &LoggingEngine::LOG_D);

    m_activeWorker->start();
    return true;
}

void LoggingEngine::stop()
{
    if (!m_activeWorker)
        return;

    // Disconnect first: a user-requested stop should not also come back through
    // handleWorkerSessionEnded's sessionEnded(StoppedByUser, ...) relay -- the
    // caller already knows it asked for this.
    m_activeWorker->disconnect(this);
    m_activeWorker->requestStop();
    m_activeWorker->wait();
    delete m_activeWorker;
    m_activeWorker = nullptr;
    m_activeProtocol.reset();
}

void LoggingEngine::handleWorkerSessionEnded(SessionEndReason reason, QString message)
{
    emit sessionEnded(reason, message);
    if (m_activeWorker) {
        m_activeWorker->wait();
        m_activeWorker->deleteLater();
        m_activeWorker = nullptr;
        m_activeProtocol.reset();
    }
}
```

- [ ] **Step 3: Write `tests/test_logging_engine.h`**

```cpp
#pragma once
int run_test_logging_engine(int argc, char** argv);
```

- [ ] **Step 4: Write the tests in `tests/test_logging_engine.cpp`**

```cpp
#include <QtTest>
#include <QSignalSpy>
#include "logging/logging_engine.h"
#include "scripted_logging_protocol.h"
#include "test_logging_engine.h"

class TestLoggingEngine : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        qRegisterMetaType<QVector<LogSample>>();
        qRegisterMetaType<LoggingStatus>();
        qRegisterMetaType<SessionEndReason>();
    }

    void start_with_unregistered_protocol_fails_immediately() {
        LoggingEngine engine;
        LogSessionConfig config;
        config.protocolId = "NOPE";
        QVERIFY(!engine.start(config));
        QVERIFY(!engine.isRunning());
    }

    void start_and_stop_round_trip() {
        LoggingEngine engine;
        auto *proto = new ScriptedLoggingProtocol();
        proto->queueStartResult(true);
        engine.registerProtocol("TEST",
            [proto](const LogSessionConfig &) { return std::unique_ptr<LoggingProtocol>(proto); },
            20, 5, 10, 10);

        LogSessionConfig config;
        config.protocolId = "TEST";
        QVERIFY(engine.start(config));
        QVERIFY(engine.isRunning());

        engine.stop();
        QVERIFY(!engine.isRunning());
    }

    void spontaneous_handshake_failure_propagates_session_ended() {
        LoggingEngine engine;
        auto *proto = new ScriptedLoggingProtocol();
        proto->queueStartResult(false, "no ECU");
        engine.registerProtocol("TEST",
            [proto](const LogSessionConfig &) { return std::unique_ptr<LoggingProtocol>(proto); },
            20, 5, 10, 10);

        QSignalSpy endedSpy(&engine, &LoggingEngine::sessionEnded);

        LogSessionConfig config;
        config.protocolId = "TEST";
        QVERIFY(engine.start(config));
        QVERIFY(endedSpy.wait(2000));

        QCOMPARE(endedSpy.at(0).at(0).value<SessionEndReason>(), SessionEndReason::HandshakeFailed);
    }
};

int run_test_logging_engine(int argc, char** argv) {
    TestLoggingEngine t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_logging_engine.moc"
```

- [ ] **Step 5: Wire the new files into the build**

In `tests/tests.pro`, add to `SOURCES`:
```
    test_logging_engine.cpp \
    ../logging/logging_engine.cpp \
```
and to `HEADERS`:
```
    test_logging_engine.h \
    ../logging/logging_engine.h \
```

In `tests/main.cpp`, add: `status |= run_test_logging_engine(argc, argv);`

In `FastECU.pro`, add `logging/logging_engine.cpp` to `SOURCES` and `logging/logging_engine.h` to `HEADERS`.

- [ ] **Step 6: Build and run**

Run: `cd tests && qmake tests.pro && make && ./mut_dma_tests`
Expected: all previous suites plus `TestLoggingEngine` reporting 3 passed, 0 failed.

- [ ] **Step 7: Commit**

```bash
git add logging/logging_engine.h logging/logging_engine.cpp \
        tests/test_logging_engine.h tests/test_logging_engine.cpp \
        tests/tests.pro tests/main.cpp FastECU.pro
git commit -m "feat: add LoggingEngine session facade and protocol registry"
```

---

## Task 3: Transport `isOpen()` seam + new `ISsmTransport`

Adapter-loss detection (`PollResult::Status::TransportError`) needs a connectivity check that works against a *scripted* transport in tests, not a live `SerialPortActions` (a freshly-constructed, unopened `SerialPortActions::is_serial_port_open()` always returns `false` -- there is no lightweight way to make it report "open" without a real port, which would make any protocol-wrapper test that needs a successful `start()`/`poll()` impossible to write against it directly). So connectivity lives on the *transport* interfaces instead, where a scripted test double can control it directly:

- `IKlineTransport` and `ICanTransport` (existing, used by `MutDmaDriver`/`CdbgLogDriver`) each gain `virtual bool isOpen() const = 0;`. This is an additive, non-breaking change to two existing pure-virtual interfaces: every existing implementation (`FastEcuKlineTransport`, `FastEcuCanTransport`, `ScriptedKlineTransport`, `ScriptedCanTransport`) needs the new override, but `ScriptedKlineTransport`/`ScriptedCanTransport` default it to `true`, so `tests/test_driver.cpp` and `tests/test_cdbg_driver.cpp`'s existing test bodies keep passing unmodified.
- A new `ISsmTransport` (mirroring `IKlineTransport`'s shape) is added for SSM, which currently has no transport seam at all -- `log_ssm_values()`/`read_log_serial_data()` call `SerialPortActions` directly.

This gives all three protocols (Task 4-6) the same testable connectivity check: `transport_->isOpen()`, backed in production by `SerialPortActions::is_serial_port_open()` and in tests by a settable boolean on the scripted double.

**Files:**
- Modify: `protocol/ikline_transport.h`
- Modify: `protocol/ican_transport.h`
- Modify: `protocol/fastecu_kline_transport.h`
- Modify: `protocol/fastecu_kline_transport.cpp`
- Modify: `protocol/fastecu_can_transport.h`
- Modify: `protocol/fastecu_can_transport.cpp`
- Modify: `tests/scripted_kline_transport.h`
- Modify: `tests/scripted_can_transport.h`
- Create: `protocol/issm_transport.h`
- Create: `protocol/fastecu_ssm_transport.h`
- Create: `protocol/fastecu_ssm_transport.cpp`
- Create: `tests/scripted_ssm_transport.h`
- Modify: `tests/tests.pro`
- Modify: `FastECU.pro`

**Interfaces:**
- Produces: `IKlineTransport::isOpen()`, `ICanTransport::isOpen()`, `ISsmTransport{write(QByteArray)/read(int)/isOpen()}`, `FastEcuSsmTransport : public ISsmTransport` wrapping `SerialPortActions*`, `ScriptedSsmTransport : public ISsmTransport` test double with `setOpen(bool)` (default `true`).
- Consumes: `SerialPortActions` from `serial_port/serial_port_actions.h` (existing); existing `IKlineTransport`/`ICanTransport`/`FastEcuKlineTransport`/`FastEcuCanTransport`/`ScriptedKlineTransport`/`ScriptedCanTransport`.

- [ ] **Step 1: Add `isOpen()` to `protocol/ikline_transport.h`**

```cpp
#pragma once
#include <QByteArray>
namespace mutdma {
class IKlineTransport {
public:
    virtual ~IKlineTransport() = default;
    virtual bool setBaud(int baud) = 0;
    virtual int  write(const QByteArray& data) = 0;          // returns bytes written
    virtual QByteArray read(int timeoutMs, int wantBytes = -1) = 0;
    // True if the underlying adapter connection is open. Used to distinguish
    // "adapter disconnected" from "ECU not responding" in LoggingProtocol wrappers.
    virtual bool isOpen() const = 0;
};
}
```

- [ ] **Step 2: Add `isOpen()` to `protocol/ican_transport.h`**

```cpp
#pragma once
#include <QByteArray>

namespace cdbg {

// Raw-CAN transport (not ISO-TP): every write/read is one CAN frame,
// addressed by an explicit 11-bit arbitration ID.
class ICanTransport {
public:
    virtual ~ICanTransport() = default;
    // Sends `payload` (up to 8 bytes) on CAN id `canId`. Returns bytes written.
    virtual int write(quint32 canId, const QByteArray &payload) = 0;
    // Reads one CAN frame within timeoutMs. Returns the frame's payload and
    // sets outId to its CAN id, or returns an empty QByteArray if nothing
    // arrived in time.
    virtual QByteArray read(int timeoutMs, quint32 &outId) = 0;
    // True if the underlying adapter connection is open.
    virtual bool isOpen() const = 0;
};

} // namespace cdbg
```

- [ ] **Step 3: Implement `isOpen()` on `FastEcuKlineTransport`**

In `protocol/fastecu_kline_transport.h`, add `bool isOpen() const override;` to the class body. In `protocol/fastecu_kline_transport.cpp`, add:

```cpp
bool mutdma::FastEcuKlineTransport::isOpen() const
{
    return serial_->is_serial_port_open();
}
```

(Match the existing file's exact namespace/qualification style for `write`/`read` -- if those are defined as `bool mutdma::FastEcuKlineTransport::setBaud(...)` etc. without a `using namespace mutdma;` at file scope, follow that same qualified form for `isOpen()`.)

- [ ] **Step 4: Implement `isOpen()` on `FastEcuCanTransport`**

In `protocol/fastecu_can_transport.h`, add `bool isOpen() const override;` to the class body. In `protocol/fastecu_can_transport.cpp`, add:

```cpp
bool cdbg::FastEcuCanTransport::isOpen() const
{
    return serial_->is_serial_port_open();
}
```

- [ ] **Step 5: Add `isOpen()`/`setOpen()` to `tests/scripted_kline_transport.h`**

Add to the `public:` section of `ScriptedKlineTransport`:
```cpp
    void setOpen(bool open) { open_ = open; }
    bool isOpen() const override { return open_; }
```
and to `private:`:
```cpp
    bool open_ = true;
```

- [ ] **Step 6: Add `isOpen()`/`setOpen()` to `tests/scripted_can_transport.h`**

Add to the `public:` section of `ScriptedCanTransport`:
```cpp
    void setOpen(bool open) { open_ = open; }
    bool isOpen() const override { return open_; }
```
and to `private:`:
```cpp
    bool open_ = true;
```

- [ ] **Step 7: Build and run the existing test suite to confirm the additive change is non-breaking**

Run: `cd tests && qmake tests.pro && make && ./mut_dma_tests`
Expected: `TestDriver` and `TestCdbgDriver` (and every other existing suite) still pass unmodified -- `isOpen()` defaults to `true` on both scripted doubles.

- [ ] **Step 8: Write `protocol/issm_transport.h`**

```cpp
#pragma once
#include <QByteArray>

// Raw byte-stream transport for the SSM logging protocol (request/response over
// K-Line, ISO14230, or CAN depending on vehicle configuration -- the wire framing
// differences are handled by SerialPortActions itself, not by this seam).
class ISsmTransport {
public:
    virtual ~ISsmTransport() = default;
    // Returns bytes written.
    virtual int write(const QByteArray &data) = 0;
    // Reads whatever is available within timeoutMs. Returns an empty QByteArray
    // if nothing arrived in time.
    virtual QByteArray read(int timeoutMs) = 0;
    // True if the underlying adapter connection is open.
    virtual bool isOpen() const = 0;
};
```

- [ ] **Step 9: Write `protocol/fastecu_ssm_transport.h`**

```cpp
#pragma once
#include "protocol/issm_transport.h"
class SerialPortActions;

// Adapts FastECU's SerialPortActions to ISsmTransport.
class FastEcuSsmTransport : public ISsmTransport {
public:
    explicit FastEcuSsmTransport(SerialPortActions *serial) : serial_(serial) {}
    int write(const QByteArray &data) override;
    QByteArray read(int timeoutMs) override;
    bool isOpen() const override;

private:
    SerialPortActions *serial_;
};
```

- [ ] **Step 10: Write `protocol/fastecu_ssm_transport.cpp`**

```cpp
#include "protocol/fastecu_ssm_transport.h"
#include "serial_port/serial_port_actions.h"

int FastEcuSsmTransport::write(const QByteArray &data)
{
    serial_->write_serial_data_echo_check(data);
    return data.size();
}

QByteArray FastEcuSsmTransport::read(int timeoutMs)
{
    return serial_->read_serial_data(static_cast<uint16_t>(timeoutMs));
}

bool FastEcuSsmTransport::isOpen() const
{
    return serial_->is_serial_port_open();
}
```

- [ ] **Step 11: Write `tests/scripted_ssm_transport.h`**

```cpp
#pragma once
#include "protocol/issm_transport.h"
#include <QList>

// Test double: assert the exact sequence of writes, feed canned reads in order.
// Mirrors tests/scripted_kline_transport.h's shape.
class ScriptedSsmTransport : public ISsmTransport {
public:
    void expectWrite(const QByteArray &b) { expected_.append(b); }
    void queueRead(const QByteArray &b) { reads_.append(b); }
    bool scriptConsumed() const { return wIdx_ == expected_.size() && rIdx_ == reads_.size(); }
    bool ok() const { return ok_; }
    void setOpen(bool open) { open_ = open; }
    bool isOpen() const override { return open_; }

    int write(const QByteArray &data) override {
        if (wIdx_ >= expected_.size() || expected_.at(wIdx_) != data)
            ok_ = false;
        else
            ++wIdx_;
        return data.size();
    }

    QByteArray read(int) override {
        if (rIdx_ >= reads_.size())
            return QByteArray();
        return reads_.at(rIdx_++);
    }

private:
    QList<QByteArray> expected_, reads_;
    int wIdx_ = 0, rIdx_ = 0;
    bool ok_ = true;
    bool open_ = true;
};
```

- [ ] **Step 12: Wire the new files into the build**

In `tests/tests.pro`, add to `SOURCES`:
```
    ../protocol/fastecu_ssm_transport.cpp \
```
and to `HEADERS`:
```
    scripted_ssm_transport.h \
    ../protocol/issm_transport.h \
    ../protocol/fastecu_ssm_transport.h \
```

In `FastECU.pro`, add `protocol/fastecu_ssm_transport.cpp` to `SOURCES` and `protocol/issm_transport.h`, `protocol/fastecu_ssm_transport.h` to `HEADERS`.

- [ ] **Step 13: Build the test binary to confirm it still compiles**

Run: `cd tests && qmake tests.pro && make`
Expected: builds cleanly (no new tests reference `ISsmTransport` yet -- that's Task 4).

- [ ] **Step 14: Commit**

```bash
git add protocol/ikline_transport.h protocol/ican_transport.h \
        protocol/fastecu_kline_transport.h protocol/fastecu_kline_transport.cpp \
        protocol/fastecu_can_transport.h protocol/fastecu_can_transport.cpp \
        tests/scripted_kline_transport.h tests/scripted_can_transport.h \
        protocol/issm_transport.h protocol/fastecu_ssm_transport.h protocol/fastecu_ssm_transport.cpp \
        tests/scripted_ssm_transport.h tests/tests.pro FastECU.pro
git commit -m "feat: add isOpen() transport seam and new ISsmTransport for adapter-loss detection"
```

---

## Task 4: `SsmLoggingProtocol` port + tests

Ports `log_ssm_values()`, `read_log_serial_data()`, and the per-cycle parts of `parse_log_params()` from `log_operations_ssm.cpp` (lines 291-347, 349-393, 395-507). Two deliberate behavior changes vs. the original, both required by the new architecture and noted here for the reviewer:

1. **The per-cycle request is now actually re-sent every poll.** In the current code, `logging_poll_timer` (which would call `log_ssm_values()` again each cycle) is created and connected but **never started** anywhere in the codebase (confirmed via `grep -rn "logging_poll_timer" *.cpp` -- only `->stop()` call sites exist). `log_ssm_values()` is called exactly once when logging starts, so today's SSM path never actually issues a second per-cycle request. `SsmLoggingProtocol::poll()` sends the "continue" request (`0xA8 0x01 <addresses>`) on every call, matching what the request-building code was clearly meant to do and what "polling type" (send the next request as soon as the previous response arrives) requires.
2. **The read/reassembly loop is bounded by the caller's `timeoutMs`.** The original `read_log_serial_data()`'s non-openport2 branch has worst-case loop bounds (100 x 10ms, then 200 x 50ms, then +50ms +100ms) that total over 11 seconds -- far larger than any reasonable poll cycle, and incompatible with `LoggingWorker` noticing a stop request promptly. `SsmLoggingProtocol` preserves the same reassembly strategy (search for the `0x80 0xf0 0x10` header by dropping leading bytes) but bounds the whole loop by a `QElapsedTimer` against `timeoutMs`.

Also dropped: `parse_log_params()`'s `log_speed`/`log_speed_timer` computation, which is dead code (the only place its result is used is a commented-out `emit LOG_D(...)` line).

**Files:**
- Create: `logging/protocols/ssm_logging_protocol.h`
- Create: `logging/protocols/ssm_logging_protocol.cpp`
- Create: `tests/test_ssm_logging_protocol.h`
- Create: `tests/test_ssm_logging_protocol.cpp`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`
- Modify: `FastECU.pro`

**Interfaces:**
- Consumes: `ISsmTransport` (Task 3, including `isOpen()`), `LoggingProtocol`/`PollResult`/`LogSample` (Task 1), `ScriptedSsmTransport` (Task 3), `FileActions::LogValuesStructure`/`FileActions` (existing, `file_actions.h`).
- Produces: `class SsmLoggingProtocol : public LoggingProtocol` with constructor `SsmLoggingProtocol(std::unique_ptr<ISsmTransport> transport, FileActions::LogValuesStructure *logValues, FileActions *fileActions, QString logValueProtocolFilter, bool targetIsEcu, bool useOpenport2Adapter)`. Note: no `SerialPortActions*` parameter -- connectivity is checked via `transport_->isOpen()` instead (see Task 3), and `useOpenport2Adapter`/`targetIsEcu` are captured once as plain booleans at construction rather than queried live, so this class has no dependency on `SerialPortActions` at all.

- [ ] **Step 1: Write `logging/protocols/ssm_logging_protocol.h`**

```cpp
#pragma once
#include <memory>
#include <QString>
#include "logging/logging_protocol.h"
#include "protocol/issm_transport.h"
#include <file_actions.h>

class SsmLoggingProtocol : public LoggingProtocol {
public:
    SsmLoggingProtocol(std::unique_ptr<ISsmTransport> transport,
                        FileActions::LogValuesStructure *logValues, FileActions *fileActions,
                        QString logValueProtocolFilter, bool targetIsEcu, bool useOpenport2Adapter);

    bool start(QString *errorOut) override;
    PollResult poll(int timeoutMs) override;
    void stop() override;

private:
    QByteArray buildSsmHeader(QByteArray output) const;
    uint8_t ssmChecksum(const QByteArray &output) const;
    QByteArray readFramedResponse(int timeoutMs);

    std::unique_ptr<ISsmTransport> transport_;
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    QString logValueProtocolFilter_;
    bool targetIsEcu_;
    bool useOpenport2Adapter_;
};
```

- [ ] **Step 2: Write `logging/protocols/ssm_logging_protocol.cpp`**

```cpp
#include "logging/protocols/ssm_logging_protocol.h"
#include <QElapsedTimer>
#include <QRegularExpression>

namespace {
constexpr int kStartTimeoutMs = 1000;
}

SsmLoggingProtocol::SsmLoggingProtocol(std::unique_ptr<ISsmTransport> transport,
                                        FileActions::LogValuesStructure *logValues, FileActions *fileActions,
                                        QString logValueProtocolFilter, bool targetIsEcu, bool useOpenport2Adapter)
    : transport_(std::move(transport))
    , logValues_(logValues)
    , fileActions_(fileActions)
    , logValueProtocolFilter_(std::move(logValueProtocolFilter))
    , targetIsEcu_(targetIsEcu)
    , useOpenport2Adapter_(useOpenport2Adapter)
{
}

uint8_t SsmLoggingProtocol::ssmChecksum(const QByteArray &output) const
{
    uint8_t checksum = 0;
    for (int i = 0; i < output.length(); i++)
        checksum += (uint8_t)output.at(i);
    return checksum;
}

QByteArray SsmLoggingProtocol::buildSsmHeader(QByteArray output) const
{
    uint8_t length = (uint8_t)output.length();
    output.insert(0, (uint8_t)0x80);
    output.insert(1, targetIsEcu_ ? (uint8_t)0x10 : (uint8_t)0x18);
    output.insert(2, (uint8_t)0xF0);
    output.insert(3, length);
    output.append((char)ssmChecksum(output));
    return output;
}

QByteArray SsmLoggingProtocol::readFramedResponse(int timeoutMs)
{
    QByteArray received;
    QElapsedTimer clock;
    clock.start();

    if (useOpenport2Adapter_)
        return transport_->read(timeoutMs);

    while (received.length() < 3 && clock.elapsed() < timeoutMs)
        received.append(transport_->read(10));

    while (received.length() >= 3
           && ((uint8_t)received.at(0) != 0x80 || (uint8_t)received.at(1) != 0xf0 || (uint8_t)received.at(2) != 0x10)
           && clock.elapsed() < timeoutMs) {
        received.remove(0, 1);
        received.append(transport_->read(10));
    }

    int remaining = timeoutMs - int(clock.elapsed());
    if (remaining > 0)
        received.append(transport_->read(remaining));

    return received;
}

bool SsmLoggingProtocol::start(QString *errorOut)
{
    if (!transport_->isOpen()) {
        if (errorOut) *errorOut = "adapter disconnected";
        return false;
    }

    QByteArray output;
    output.append((uint8_t)0xA8);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    transport_->write(buildSsmHeader(output));

    QByteArray received = readFramedResponse(kStartTimeoutMs);
    if (received.length() <= 6 || (uint8_t)received.at(4) != 0xe8) {
        if (errorOut) *errorOut = "no response to logging start request";
        return false;
    }
    return true;
}

PollResult SsmLoggingProtocol::poll(int timeoutMs)
{
    PollResult result;

    if (!transport_->isOpen()) {
        result.status = PollResult::Status::TransportError;
        result.errorMessage = "adapter disconnected";
        return result;
    }

    QByteArray output;
    output.append((uint8_t)0xA8);
    output.append((uint8_t)0x01);
    bool ok = false;
    for (int i = 0; i < logValues_->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < logValues_->log_value_id.length(); j++) {
            if (logValues_->lower_panel_log_value_id.at(i) == logValues_->log_value_id.at(j)
                && logValues_->log_value_protocol.at(j) == logValueProtocolFilter_) {
                output.append((uint8_t)(logValues_->log_value_address.at(j).toUInt(&ok, 16) >> 16));
                output.append((uint8_t)(logValues_->log_value_address.at(j).toUInt(&ok, 16) >> 8));
                output.append((uint8_t)logValues_->log_value_address.at(j).toUInt(&ok, 16));
            }
        }
    }
    transport_->write(buildSsmHeader(output));

    QByteArray received = readFramedResponse(timeoutMs);
    if (received.length() <= 6 || (uint8_t)received.at(4) != 0xe8) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    received.remove(0, 5);
    received.remove(received.length() - 1, 1);

    for (int i = 0; i < logValues_->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < logValues_->log_value_id.length(); j++) {
            if (logValues_->lower_panel_log_value_id.at(i) == logValues_->log_value_id.at(j)
                && logValues_->log_value_protocol.at(j) == logValueProtocolFilter_
                && logValues_->log_value_enabled.at(j) == "1"
                && i < received.length()) {
                QStringList conversion = logValues_->log_value_units.at(j).split(",");
                QString from_byte = conversion.at(2);
                QStringList format_str_lst = conversion.at(3).split(".");
                uint8_t format = 0;
                if (format_str_lst.length() > 1)
                    format = format_str_lst.at(1).count(QRegularExpression("0"));

                QString value;
                uint8_t length = logValues_->log_value_length.at(j).toUInt();
                for (uint8_t k = 0; k < length && (i + k) < (uint8_t)received.length(); k++)
                    value.append(QString::number((uint8_t)received.at(i + k)));

                float calc_float_value = fileActions_->calculate_value_from_expression(
                    fileActions_->parse_stringlist_from_expression_string(from_byte, value));
                QString calc_value = QString::number(calc_float_value, 'f', format);
                result.samples.append(LogSample{j, calc_value});
            }
        }
    }

    result.status = PollResult::Status::Ok;
    return result;
}

void SsmLoggingProtocol::stop()
{
    // No explicit "stop logging" wire command in the original protocol; the ECU
    // simply stops being polled once this object is destroyed.
}
```

- [ ] **Step 3: Write `tests/test_ssm_logging_protocol.h`**

```cpp
#pragma once
int run_test_ssm_logging_protocol(int argc, char** argv);
```

- [ ] **Step 4: Write the tests in `tests/test_ssm_logging_protocol.cpp`**

```cpp
#include <QtTest>
#include <file_actions.h>
#include "logging/protocols/ssm_logging_protocol.h"
#include "scripted_ssm_transport.h"
#include "test_ssm_logging_protocol.h"

namespace {
FileActions::LogValuesStructure makeOneChannel()
{
    FileActions::LogValuesStructure lv;
    lv.lower_panel_log_value_id << "P1";
    lv.log_value_id << "P1";
    lv.log_value_protocol << "SSM";
    lv.log_value_address << "1000";
    lv.log_value_units << "raw,unit,x,0";
    lv.log_value_length << "1";
    lv.log_value_enabled << "1";
    lv.log_value << "0";
    return lv;
}

QByteArray buildResponse(const QByteArray &payload)
{
    // [0x80][0xf0][0x10][len][0xe8][payload...][checksum]
    QByteArray msg;
    msg.append((char)0x80);
    msg.append((char)0xf0);
    msg.append((char)0x10);
    msg.append((char)(payload.size() + 1));
    msg.append((char)0xe8);
    msg.append(payload);
    uint8_t sum = 0;
    for (char c : msg)
        sum += (uint8_t)c;
    msg.append((char)sum);
    return msg;
}
}

class TestSsmLoggingProtocol : public QObject {
    Q_OBJECT
private slots:
    void start_succeeds_on_well_formed_response() {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->queueRead(buildResponse(QByteArray(3, char(0))));
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();

        SsmLoggingProtocol proto(std::move(transport), &lv, &fileActions, "SSM", true, false);
        QString err;
        QVERIFY(proto.start(&err));
    }

    void start_fails_on_short_response() {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->queueRead(QByteArray());
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();

        SsmLoggingProtocol proto(std::move(transport), &lv, &fileActions, "SSM", true, false);
        QString err;
        QVERIFY(!proto.start(&err));
        QVERIFY(!err.isEmpty());
    }

    void start_fails_when_adapter_is_closed() {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->setOpen(false);
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();

        SsmLoggingProtocol proto(std::move(transport), &lv, &fileActions, "SSM", true, false);
        QString err;
        QVERIFY(!proto.start(&err));
        QCOMPARE(err, QString("adapter disconnected"));
    }

    void poll_decodes_one_channel() {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->queueRead(buildResponse(QByteArray(1, char(42))));
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();

        SsmLoggingProtocol proto(std::move(transport), &lv, &fileActions, "SSM", true, false);
        PollResult r = proto.poll(200);

        QCOMPARE((int)r.status, (int)PollResult::Status::Ok);
        QCOMPARE(r.samples.size(), 1);
        QCOMPARE(r.samples.at(0).logValueIndex, 0);
        QCOMPARE(r.samples.at(0).displayValue, QString("42"));
    }

    void poll_returns_no_response_on_timeout() {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();

        SsmLoggingProtocol proto(std::move(transport), &lv, &fileActions, "SSM", true, false);
        PollResult r = proto.poll(50);

        QCOMPARE((int)r.status, (int)PollResult::Status::NoResponse);
    }

    void poll_returns_transport_error_when_adapter_closes_mid_session() {
        auto transport = std::make_unique<ScriptedSsmTransport>();
        transport->queueRead(buildResponse(QByteArray(1, char(1))));
        FileActions fileActions;
        FileActions::LogValuesStructure lv = makeOneChannel();
        ScriptedSsmTransport *raw = transport.get();

        SsmLoggingProtocol proto(std::move(transport), &lv, &fileActions, "SSM", true, false);
        QString err;
        QVERIFY(proto.start(&err));

        raw->setOpen(false);
        PollResult r = proto.poll(50);
        QCOMPARE((int)r.status, (int)PollResult::Status::TransportError);
    }
};

int run_test_ssm_logging_protocol(int argc, char** argv) {
    TestSsmLoggingProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_ssm_logging_protocol.moc"
```

- [ ] **Step 5: Wire the new files into the build**

In `tests/tests.pro`, add to `SOURCES`:
```
    test_ssm_logging_protocol.cpp \
    ../logging/protocols/ssm_logging_protocol.cpp \
```
and to `HEADERS`:
```
    test_ssm_logging_protocol.h \
    ../logging/protocols/ssm_logging_protocol.h \
```

In `tests/main.cpp`, add: `status |= run_test_ssm_logging_protocol(argc, argv);`

In `FastECU.pro`, add `logging/protocols/ssm_logging_protocol.cpp` to `SOURCES` and `logging/protocols/ssm_logging_protocol.h` to `HEADERS`.

- [ ] **Step 6: Build and run**

Run: `cd tests && qmake tests.pro && make && ./mut_dma_tests`
Expected: all previous suites plus `TestSsmLoggingProtocol` reporting 6 passed, 0 failed.

- [ ] **Step 7: Commit**

```bash
git add logging/protocols/ssm_logging_protocol.h logging/protocols/ssm_logging_protocol.cpp \
        tests/test_ssm_logging_protocol.h tests/test_ssm_logging_protocol.cpp \
        tests/tests.pro tests/main.cpp FastECU.pro
git commit -m "feat: port SSM logging to SsmLoggingProtocol"
```

---

## Task 5: `MutDmaLoggingProtocol` port + tests

Thin adapter over the existing `MutDmaDriver`, porting `mitsubishi_dma_start_logging()`/`mitsubishi_dma_poll()`/`mitsubishi_dma_stop_logging()` from `log_operations_mitsubishi.cpp`.

**Files:**
- Create: `logging/protocols/mut_dma_logging_protocol.h`
- Create: `logging/protocols/mut_dma_logging_protocol.cpp`
- Create: `tests/test_mut_dma_logging_protocol.h`
- Create: `tests/test_mut_dma_logging_protocol.cpp`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`
- Modify: `FastECU.pro`

**Interfaces:**
- Consumes: `mutdma::MutDmaDriver`, `mutdma::Channel` (`protocol/mut_dma_driver.h`, `protocol/mut_dma_freeform.h`, existing), `mutdma::IKlineTransport` (Task 3, including `isOpen()`), `mutdma::IMutDmaInit` (existing), `ScriptedKlineTransport` (`tests/scripted_kline_transport.h`, Task 3), `LoggingProtocol`/`PollResult`/`LogSample` (Task 1).
- Produces: `class MutDmaLoggingProtocol : public LoggingProtocol` with constructor `MutDmaLoggingProtocol(std::unique_ptr<mutdma::IKlineTransport> transport, std::unique_ptr<mutdma::IMutDmaInit> init, FileActions::LogValuesStructure *logValues, FileActions *fileActions)`. Note: no `SerialPortActions*` parameter -- the original `mitsubishi_dma_start_logging()`/`poll()` never called any `SerialPortActions` method directly (only through the transport), so connectivity is checked via `transport_->isOpen()` alone.

- [ ] **Step 1: Write `logging/protocols/mut_dma_logging_protocol.h`**

```cpp
#pragma once
#include <memory>
#include "logging/logging_protocol.h"
#include "protocol/ikline_transport.h"
#include "protocol/imut_dma_init.h"
#include "protocol/mut_dma_driver.h"
#include <file_actions.h>

class MutDmaLoggingProtocol : public LoggingProtocol {
public:
    MutDmaLoggingProtocol(std::unique_ptr<mutdma::IKlineTransport> transport,
                           std::unique_ptr<mutdma::IMutDmaInit> init,
                           FileActions::LogValuesStructure *logValues, FileActions *fileActions);

    bool start(QString *errorOut) override;
    PollResult poll(int timeoutMs) override;
    void stop() override;

private:
    std::unique_ptr<mutdma::IKlineTransport> transport_;
    std::unique_ptr<mutdma::IMutDmaInit> init_;
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    mutdma::MutDmaDriver driver_;
    QVector<mutdma::Channel> channels_;
    QVector<int> channelLogValueIndex_;
};
```

- [ ] **Step 2: Write `logging/protocols/mut_dma_logging_protocol.cpp`**

```cpp
#include "logging/protocols/mut_dma_logging_protocol.h"
#include <QRegularExpression>

using namespace mutdma;

namespace {
// Build the free-form channel list from the enabled MUT_DMA log values, in the
// same order the display path consumes them. outIndices[i] is the index j into
// logValues->log_value_* for the channel returned at position i.
QVector<Channel> channelsFromLogValues(FileActions::LogValuesStructure *lv, QVector<int> &outIndices)
{
    QVector<Channel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < lv->log_value_id.length(); j++) {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j)
                && lv->log_value_protocol.at(j) == "MUT_DMA"
                && lv->log_value_enabled.at(j) == "1") {
                Channel c;
                c.id = quint16(lv->log_value_address.at(j).toUInt(nullptr, 16));
                c.len = quint8(lv->log_value_length.at(j).toUInt());
                ch.append(c);
                outIndices.append(j);
            }
        }
    }
    return ch;
}
}

MutDmaLoggingProtocol::MutDmaLoggingProtocol(std::unique_ptr<IKlineTransport> transport,
                                              std::unique_ptr<IMutDmaInit> init,
                                              FileActions::LogValuesStructure *logValues, FileActions *fileActions)
    : transport_(std::move(transport))
    , init_(std::move(init))
    , logValues_(logValues)
    , fileActions_(fileActions)
    , driver_(*transport_, *init_)
{
}

bool MutDmaLoggingProtocol::start(QString *errorOut)
{
    if (!transport_->isOpen()) {
        if (errorOut) *errorOut = "adapter disconnected";
        return false;
    }

    channels_ = channelsFromLogValues(logValues_, channelLogValueIndex_);
    if (!driver_.startFreeFormLog(channels_, 0xA0, 0xA1)) {
        if (errorOut) *errorOut = "MUT/DMA free-form handshake failed";
        return false;
    }
    return true;
}

PollResult MutDmaLoggingProtocol::poll(int timeoutMs)
{
    PollResult result;

    if (!transport_->isOpen()) {
        result.status = PollResult::Status::TransportError;
        result.errorMessage = "adapter disconnected";
        return result;
    }
    if (!driver_.isStreaming()) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    QVector<quint32> vals = driver_.pollOnce(timeoutMs);
    if (vals.isEmpty()) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    for (int i = 0; i < vals.size() && i < channelLogValueIndex_.size(); ++i) {
        int j = channelLogValueIndex_.at(i);

        QStringList conversion = logValues_->log_value_units.at(j).split(",");
        QString from_byte = conversion.at(2);
        QStringList format_str_lst = conversion.at(3).split(".");
        uint8_t format = 0;
        if (format_str_lst.length() > 1)
            format = format_str_lst.at(1).count(QRegularExpression("0"));

        QString value = QString::number(vals.at(i));
        float calc_float_value = fileActions_->calculate_value_from_expression(
            fileActions_->parse_stringlist_from_expression_string(from_byte, value));
        QString calc_value = QString::number(calc_float_value, 'f', format);
        result.samples.append(LogSample{j, calc_value});
    }

    result.status = PollResult::Status::Ok;
    return result;
}

void MutDmaLoggingProtocol::stop()
{
    // MutDmaDriver has no explicit "stop streaming" wire command; the ECU's
    // stream simply stops being read once this protocol/driver is destroyed.
}
```

- [ ] **Step 3: Write `tests/test_mut_dma_logging_protocol.h`**

```cpp
#pragma once
int run_test_mut_dma_logging_protocol(int argc, char** argv);
```

- [ ] **Step 4: Write the tests in `tests/test_mut_dma_logging_protocol.cpp`**

```cpp
#include <QtTest>
#include <file_actions.h>
#include "logging/protocols/mut_dma_logging_protocol.h"
#include "protocol/mut_dma_codec.h"
#include "scripted_kline_transport.h"
#include "test_mut_dma_logging_protocol.h"

using namespace mutdma;

namespace {
FileActions::LogValuesStructure makeOneChannel()
{
    FileActions::LogValuesStructure lv;
    lv.lower_panel_log_value_id << "M1";
    lv.log_value_id << "M1";
    lv.log_value_protocol << "MUT_DMA";
    lv.log_value_address << "8000";
    lv.log_value_units << "raw,unit,x,0";
    lv.log_value_length << "2";
    lv.log_value_enabled << "1";
    lv.log_value << "0";
    return lv;
}
}

class TestMutDmaLoggingProtocol : public QObject {
    Q_OBJECT
private slots:
    void start_reaches_streaming_on_valid_handshake() {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        FileActions::LogValuesStructure lv = makeOneChannel();
        QVector<Channel> ch = { {0x8000, 2} };
        transport->expectWrite(buildSetupFrame(0xA0, 1));
        transport->queueRead(buildCommandFrame(0xA5, QByteArray(), TRAILER_STD));
        transport->expectWrite(buildIdListFrame(0xA1, ch));
        transport->queueRead(buildCommandFrame(0x05, QByteArray(), TRAILER_STD));

        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);
        QString err;
        QVERIFY(proto.start(&err));
    }

    void start_fails_when_adapter_is_closed() {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        transport->setOpen(false);
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);

        QString err;
        QVERIFY(!proto.start(&err));
        QCOMPARE(err, QString("adapter disconnected"));
    }

    void poll_returns_no_response_before_start() {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);

        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::NoResponse);
    }

    void poll_returns_transport_error_when_adapter_closed() {
        auto transport = std::make_unique<ScriptedKlineTransport>();
        transport->setOpen(false);
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        MutDmaLoggingProtocol proto(std::move(transport), std::make_unique<AlreadyInMode>(125000), &lv, &fileActions);

        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::TransportError);
    }
};

int run_test_mut_dma_logging_protocol(int argc, char** argv) {
    TestMutDmaLoggingProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_mut_dma_logging_protocol.moc"
```

- [ ] **Step 5: Wire the new files into the build**

In `tests/tests.pro`, add to `SOURCES`:
```
    test_mut_dma_logging_protocol.cpp \
    ../logging/protocols/mut_dma_logging_protocol.cpp \
```
and to `HEADERS`:
```
    test_mut_dma_logging_protocol.h \
    ../logging/protocols/mut_dma_logging_protocol.h \
```

In `tests/main.cpp`, add: `status |= run_test_mut_dma_logging_protocol(argc, argv);`

In `FastECU.pro`, add `logging/protocols/mut_dma_logging_protocol.cpp` to `SOURCES` and `logging/protocols/mut_dma_logging_protocol.h` to `HEADERS`.

- [ ] **Step 6: Build and run**

Run: `cd tests && qmake tests.pro && make && ./mut_dma_tests`
Expected: all previous suites plus `TestMutDmaLoggingProtocol` reporting 4 passed, 0 failed.

- [ ] **Step 7: Commit**

```bash
git add logging/protocols/mut_dma_logging_protocol.h logging/protocols/mut_dma_logging_protocol.cpp \
        tests/test_mut_dma_logging_protocol.h tests/test_mut_dma_logging_protocol.cpp \
        tests/tests.pro tests/main.cpp FastECU.pro
git commit -m "feat: port MUT/DMA logging to MutDmaLoggingProtocol"
```

---

## Task 6: `CdbgLoggingProtocol` port + tests

Thin adapter over the existing `CdbgLogDriver`, porting `cdbg_start_logging()`/`cdbg_poll()`/`cdbg_stop_logging()` from `log_operations_mitsubishi_cdbg.cpp`. Unlike the other two protocols, `start()` also configures the serial port for raw CAN mode (this used to run once, synchronously, on the GUI thread when `cdbg_start_logging()` was called from `toggle_realtime()`; it now runs on the worker thread instead, inside `LoggingWorker::run()`'s call to `protocol->start()`).

**Files:**
- Create: `logging/protocols/cdbg_logging_protocol.h`
- Create: `logging/protocols/cdbg_logging_protocol.cpp`
- Create: `tests/test_cdbg_logging_protocol.h`
- Create: `tests/test_cdbg_logging_protocol.cpp`
- Modify: `tests/tests.pro`
- Modify: `tests/main.cpp`
- Modify: `FastECU.pro`

**Interfaces:**
- Consumes: `MitsuColtCanCdbg::CdbgLogDriver`, `MitsuColtCanCdbg::CdbgChannel`, `MitsuColtCanCdbg::kReplyCanId` (`protocol/mitsu_colt_can_cdbg_driver.h`, `protocol/mitsu_colt_can_cdbg_protocol.h`, existing), `cdbg::ICanTransport` (Task 3, including `isOpen()`), `ScriptedCanTransport` (`tests/scripted_can_transport.h`, Task 3), `LoggingProtocol`/`PollResult`/`LogSample` (Task 1).
- Produces: `class CdbgLoggingProtocol : public LoggingProtocol` with constructor `CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, SerialPortActions *serial, FileActions::LogValuesStructure *logValues, FileActions *fileActions)`. Unlike SSM/MUT-DMA, this one keeps `SerialPortActions*` because `start()` still needs to call the real CAN-mode config setters (`set_is_can_connection`, `set_can_speed`, ...) and `open_serial_port()` -- but connectivity (`TransportError`) is still checked via `transport_->isOpen()`, not `serial_->is_serial_port_open()`, for the same testability reason as the other two protocols.

- [ ] **Step 1: Write `logging/protocols/cdbg_logging_protocol.h`**

```cpp
#pragma once
#include <memory>
#include "logging/logging_protocol.h"
#include "protocol/ican_transport.h"
#include "protocol/mitsu_colt_can_cdbg_driver.h"
#include <file_actions.h>

class SerialPortActions;

class CdbgLoggingProtocol : public LoggingProtocol {
public:
    CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, SerialPortActions *serial,
                         FileActions::LogValuesStructure *logValues, FileActions *fileActions);

    bool start(QString *errorOut) override;
    PollResult poll(int timeoutMs) override;
    void stop() override;

private:
    std::unique_ptr<cdbg::ICanTransport> transport_;
    SerialPortActions *serial_;
    FileActions::LogValuesStructure *logValues_;
    FileActions *fileActions_;
    MitsuColtCanCdbg::CdbgLogDriver driver_;
    QVector<MitsuColtCanCdbg::CdbgChannel> channels_;
    QVector<int> channelLogValueIndex_;
};
```

- [ ] **Step 2: Write `logging/protocols/cdbg_logging_protocol.cpp`**

```cpp
#include "logging/protocols/cdbg_logging_protocol.h"
#include "serial_port/serial_port_actions.h"
#include <QRegularExpression>

using namespace MitsuColtCanCdbg;

namespace {
QVector<CdbgChannel> channelsFromLogValues(FileActions::LogValuesStructure *lv, QVector<int> &outIndices)
{
    QVector<CdbgChannel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < lv->log_value_id.length(); j++) {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j)
                && lv->log_value_protocol.at(j) == "CDBG"
                && lv->log_value_enabled.at(j) == "1") {
                CdbgChannel c;
                c.pointer = lv->log_value_address.at(j).toUInt(nullptr, 16);
                c.size = quint8(lv->log_value_length.at(j).toUInt());
                ch.append(c);
                outIndices.append(j);
            }
        }
    }
    return ch;
}
}

CdbgLoggingProtocol::CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, SerialPortActions *serial,
                                          FileActions::LogValuesStructure *logValues, FileActions *fileActions)
    : transport_(std::move(transport))
    , serial_(serial)
    , logValues_(logValues)
    , fileActions_(fileActions)
    , driver_(*transport_)
{
}

bool CdbgLoggingProtocol::start(QString *errorOut)
{
    serial_->set_is_iso14230_connection(false);
    serial_->set_add_iso14230_header(false);
    serial_->set_is_can_connection(true);
    serial_->set_is_iso15765_connection(false);
    serial_->set_is_29_bit_id(false);
    serial_->set_can_speed("500000");
    serial_->set_can_destination_address(kReplyCanId);
    serial_->open_serial_port();

    if (!transport_->isOpen()) {
        if (errorOut) *errorOut = "adapter disconnected";
        return false;
    }

    channels_ = channelsFromLogValues(logValues_, channelLogValueIndex_);
    if (!driver_.startFreeFormLog(channels_)) {
        if (errorOut) *errorOut = "Cdbg session/security handshake failed";
        return false;
    }
    return true;
}

PollResult CdbgLoggingProtocol::poll(int timeoutMs)
{
    PollResult result;

    if (!transport_->isOpen()) {
        result.status = PollResult::Status::TransportError;
        result.errorMessage = "adapter disconnected";
        return result;
    }
    if (!driver_.isStreaming()) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    QVector<quint32> vals = driver_.pollOnce(timeoutMs);
    if (vals.isEmpty()) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    for (int i = 0; i < vals.size() && i < channelLogValueIndex_.size(); ++i) {
        int j = channelLogValueIndex_.at(i);

        QStringList conversion = logValues_->log_value_units.at(j).split(",");
        QString from_byte = conversion.at(2);
        QStringList format_str_lst = conversion.at(3).split(".");
        uint8_t format = 0;
        if (format_str_lst.length() > 1)
            format = format_str_lst.at(1).count(QRegularExpression("0"));

        QString value = QString::number(vals.at(i));
        double calc_float_value = fileActions_->calculate_value_from_expression(
            fileActions_->parse_stringlist_from_expression_string(from_byte, value));
        QString calc_value = QString::number(calc_float_value, 'f', format);
        result.samples.append(LogSample{j, calc_value});
    }

    result.status = PollResult::Status::Ok;
    return result;
}

void CdbgLoggingProtocol::stop()
{
    // CdbgLogDriver has no explicit "stop streaming" wire command; the ECU's
    // stream simply stops being read once this protocol/driver is destroyed.
}
```

- [ ] **Step 3: Write `tests/test_cdbg_logging_protocol.h`**

```cpp
#pragma once
int run_test_cdbg_logging_protocol(int argc, char** argv);
```

- [ ] **Step 4: Write the tests in `tests/test_cdbg_logging_protocol.cpp`**

```cpp
#include <QtTest>
#include <file_actions.h>
#include "logging/protocols/cdbg_logging_protocol.h"
#include "scripted_can_transport.h"
#include "test_cdbg_logging_protocol.h"

using namespace MitsuColtCanCdbg;

namespace {
FileActions::LogValuesStructure makeOneChannel()
{
    FileActions::LogValuesStructure lv;
    lv.lower_panel_log_value_id << "C1";
    lv.log_value_id << "C1";
    lv.log_value_protocol << "CDBG";
    lv.log_value_address << "804000";
    lv.log_value_units << "raw,unit,x,0";
    lv.log_value_length << "1";
    lv.log_value_enabled << "1";
    lv.log_value << "0";
    return lv;
}
}

class TestCdbgLoggingProtocol : public QObject {
    Q_OBJECT
private slots:
    void poll_returns_no_response_before_start() {
        auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        SerialPortActions serial;
        CdbgLoggingProtocol proto(std::move(transport), &serial, &lv, &fileActions);

        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::NoResponse);
    }

    void poll_returns_transport_error_when_adapter_closed() {
        auto transport = std::make_unique<cdbg::ScriptedCanTransport>();
        transport->setOpen(false);
        FileActions::LogValuesStructure lv = makeOneChannel();
        FileActions fileActions;
        SerialPortActions serial;
        CdbgLoggingProtocol proto(std::move(transport), &serial, &lv, &fileActions);

        PollResult r = proto.poll(20);
        QCOMPARE((int)r.status, (int)PollResult::Status::TransportError);
    }
};

int run_test_cdbg_logging_protocol(int argc, char** argv) {
    TestCdbgLoggingProtocol t;
    return QTest::qExec(&t, argc, argv);
}
#include "test_cdbg_logging_protocol.moc"
```

`ScriptedCanTransport` lives in namespace `cdbg` (per `tests/scripted_can_transport.h`); the `using namespace MitsuColtCanCdbg;` above does not bring it into scope, hence the explicit `cdbg::ScriptedCanTransport`. A full handshake test (mirroring `tests/test_cdbg_driver.cpp`'s scripted session-init/seed-key sequence) is not repeated here since `CdbgLogDriver`'s handshake logic is already covered by `test_cdbg_driver.cpp` -- this file only needs to prove `CdbgLoggingProtocol`'s adapter logic (state before `start()`, error propagation) is correct. Neither test here calls `proto.start()`, because unlike `SsmLoggingProtocol`/`MutDmaLoggingProtocol`, `CdbgLoggingProtocol::start()` calls the real `SerialPortActions::open_serial_port()` (needed to configure raw CAN mode on the live connection) -- a real OS-level side effect not appropriate to trigger from a headless unit test. This is a known, deliberate coverage gap in the unit suite; `CdbgLoggingProtocol::start()`'s handshake path is exercised at the bench-checklist level instead (Task 10).

- [ ] **Step 5: Wire the new files into the build**

In `tests/tests.pro`, add to `SOURCES`:
```
    test_cdbg_logging_protocol.cpp \
    ../logging/protocols/cdbg_logging_protocol.cpp \
```
and to `HEADERS`:
```
    test_cdbg_logging_protocol.h \
    ../logging/protocols/cdbg_logging_protocol.h \
```

In `tests/main.cpp`, add: `status |= run_test_cdbg_logging_protocol(argc, argv);`

In `FastECU.pro`, add `logging/protocols/cdbg_logging_protocol.cpp` to `SOURCES` and `logging/protocols/cdbg_logging_protocol.h` to `HEADERS`.

- [ ] **Step 6: Build and run**

Run: `cd tests && qmake tests.pro && make && ./mut_dma_tests`
Expected: all previous suites plus `TestCdbgLoggingProtocol` reporting 2 passed, 0 failed.

- [ ] **Step 7: Commit**

```bash
git add logging/protocols/cdbg_logging_protocol.h logging/protocols/cdbg_logging_protocol.cpp \
        tests/test_cdbg_logging_protocol.h tests/test_cdbg_logging_protocol.cpp \
        tests/tests.pro tests/main.cpp FastECU.pro
git commit -m "feat: port Cdbg logging to CdbgLoggingProtocol"
```

---

## Task 7: Wire `LoggingEngine` into `MainWindow`

Adds a `LoggingEngine` member to `MainWindow`, registers the three protocols built in Tasks 4-6, and adds the GUI-thread slots that consume `LoggingEngine`'s signals. This task does not yet remove the old code paths (Task 9 does, after Task 8 rewires the call sites) -- both old and new logging paths coexist briefly so the app keeps building at every step.

**Files:**
- Modify: `mainwindow.h`
- Modify: `mainwindow.cpp`

**Interfaces:**
- Consumes: `LoggingEngine`, `LogSessionConfig` (Task 2); `SsmLoggingProtocol` (Task 4); `MutDmaLoggingProtocol` (Task 5); `CdbgLoggingProtocol` (Task 6); `FastEcuSsmTransport` (Task 3); existing `mutdma::FastEcuKlineTransport`, `mutdma::AlreadyInMode`, `cdbg::FastEcuCanTransport`.
- Produces: `MainWindow::loggingEngine` (member), `MainWindow::activeLogValueProtocolFilter` (member), `MainWindow::setupLoggingEngine()`, `MainWindow::handleLoggingValuesUpdated(QVector<LogSample>)`, `MainWindow::handleLoggingSessionEnded(SessionEndReason, QString)`.

- [ ] **Step 1: Add includes and members to `mainwindow.h`**

Add near the existing MUT/DMA includes (after line 102, `#include "protocol/fastecu_can_transport.h"`):

```cpp
#include "logging/logging_engine.h"
#include "logging/protocols/ssm_logging_protocol.h"
#include "logging/protocols/mut_dma_logging_protocol.h"
#include "logging/protocols/cdbg_logging_protocol.h"
#include "protocol/fastecu_ssm_transport.h"
```

Add near the existing `cdbg_transport`/`cdbg_driver` members (after `mainwindow.h:263`):

```cpp
    LoggingEngine *loggingEngine = nullptr;
    QString activeLogValueProtocolFilter;
```

Add method declarations near the other `log_operations` declarations (after `void cdbg_stop_logging();`):

```cpp
    void setupLoggingEngine();
```

Add slot declarations in the existing `private slots:` block (alongside `mitsubishi_dma_poll`/`cdbg_poll`):

```cpp
    void handleLoggingValuesUpdated(QVector<LogSample> samples);
    void handleLoggingSessionEnded(SessionEndReason reason, QString message);
```

- [ ] **Step 2: Implement `MainWindow::setupLoggingEngine()` in `mainwindow.cpp`**

Add this new method (e.g. at the top of `log_operations_ssm.cpp`, since that file already holds the SSM-related `MainWindow::` methods -- or as a new method at the end of `mainwindow.cpp`; place it in `mainwindow.cpp` since it wires cross-protocol state, not SSM-specific state):

```cpp
void MainWindow::setupLoggingEngine()
{
    loggingEngine = new LoggingEngine(this);

    connect(loggingEngine, &LoggingEngine::valuesUpdated, this, &MainWindow::handleLoggingValuesUpdated);
    connect(loggingEngine, &LoggingEngine::sessionEnded, this, &MainWindow::handleLoggingSessionEnded);
    connect(loggingEngine, &LoggingEngine::LOG_E, syslogger, &SystemLogger::log_messages);
    connect(loggingEngine, &LoggingEngine::LOG_W, syslogger, &SystemLogger::log_messages);
    connect(loggingEngine, &LoggingEngine::LOG_I, syslogger, &SystemLogger::log_messages);
    connect(loggingEngine, &LoggingEngine::LOG_D, syslogger, &SystemLogger::log_messages);

    loggingEngine->registerProtocol("MUT_DMA",
        [this](const LogSessionConfig &) {
            auto transport = std::make_unique<mutdma::FastEcuKlineTransport>(serial);
            auto init = std::make_unique<mutdma::AlreadyInMode>(125000);
            return std::unique_ptr<LoggingProtocol>(
                new MutDmaLoggingProtocol(std::move(transport), std::move(init), logValues, fileActions));
        },
        /*pollTimeoutMs=*/50, /*carSilenceMissThreshold=*/20,
        /*reconnectAttemptThreshold=*/100, /*reconnectRetryPeriod=*/20);

    loggingEngine->registerProtocol("CDBG",
        [this](const LogSessionConfig &) {
            auto transport = std::make_unique<cdbg::FastEcuCanTransport>(serial);
            return std::unique_ptr<LoggingProtocol>(
                new CdbgLoggingProtocol(std::move(transport), serial, logValues, fileActions));
        },
        /*pollTimeoutMs=*/50, /*carSilenceMissThreshold=*/20,
        /*reconnectAttemptThreshold=*/100, /*reconnectRetryPeriod=*/20);

    loggingEngine->registerProtocol("SSM",
        [this](const LogSessionConfig &config) {
            auto transport = std::make_unique<FastEcuSsmTransport>(serial);
            bool targetIsEcu = ecu_radio_button->isChecked();
            bool useOpenport2Adapter = serial->get_use_openport2_adapter();
            return std::unique_ptr<LoggingProtocol>(
                new SsmLoggingProtocol(std::move(transport), logValues, fileActions,
                                        config.logValueProtocolFilter, targetIsEcu, useOpenport2Adapter));
        },
        /*pollTimeoutMs=*/300, /*carSilenceMissThreshold=*/10,
        /*reconnectAttemptThreshold=*/30, /*reconnectRetryPeriod=*/10);
}
```

Call `setupLoggingEngine();` from `MainWindow`'s constructor, right after the existing `syslog_thread`/`syslogger` setup block (`mainwindow.cpp:66-79`, so `syslogger` already exists when `setupLoggingEngine()` connects to it).

- [ ] **Step 3: Implement the two new slots in `mainwindow.cpp`**

```cpp
void MainWindow::handleLoggingValuesUpdated(QVector<LogSample> samples)
{
    for (const LogSample &s : samples)
        logValues->log_value.replace(s.logValueIndex, s.displayValue);
    update_logbox_values(activeLogValueProtocolFilter);
    log_to_file();
}

void MainWindow::handleLoggingSessionEnded(SessionEndReason reason, QString message)
{
    logging_state = false;
    QList<QMenu*> menus = ui->menubar->findChildren<QMenu*>();
    foreach (QMenu *menu, menus) {
        foreach (QAction *action, menu->actions()) {
            if (action->text() == "Logging")
                action->setChecked(false);
        }
    }

    if (reason == SessionEndReason::AdapterDisconnected)
        QMessageBox::warning(this, tr("Logging"), "Logging adapter disconnected: " + message);
    else if (reason == SessionEndReason::HandshakeFailed)
        QMessageBox::warning(this, tr("Logging"), "Unable to start logging: " + message);
}
```

Note: `LoggingEngine::stop()` (Task 2) disconnects its worker's signals before requesting a stop, so `sessionEnded(StoppedByUser, ...)` never reaches `handleLoggingSessionEnded` for a user-initiated stop -- this slot only fires for the two unsolicited endings, which is why it unchecks the "Logging" menu action itself (the user-initiated stop path in Task 8 already does this via the existing `toggle_realtime()` logic).

- [ ] **Step 4: Build the app to confirm it still compiles**

Run: `qmake FastECU.pro && make`
Expected: builds cleanly. `loggingEngine` is constructed and its protocols registered, but nothing calls `loggingEngine->start()`/`stop()` yet (that's Task 8), so existing behavior is unchanged.

- [ ] **Step 5: Commit**

```bash
git add mainwindow.h mainwindow.cpp
git commit -m "feat: wire LoggingEngine into MainWindow alongside existing logging code"
```

---

## Task 8: Rewire `toggle_realtime()` and the destructor to use `LoggingEngine`

**Files:**
- Modify: `menu_actions.cpp` (`MainWindow::toggle_realtime()`, lines 890-965)
- Modify: `mainwindow.cpp` (`MainWindow::~MainWindow()`, lines 490-513)

**Interfaces:**
- Consumes: `MainWindow::loggingEngine`, `MainWindow::activeLogValueProtocolFilter` (Task 7); `LogSessionConfig` (Task 2).

- [ ] **Step 1: Replace the start branch in `menu_actions.cpp::toggle_realtime()`**

Replace (lines 922-936):
```cpp
        logging_counter = 0;
        logging_state = true;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_start_logging();
        }
        else if (configValues->flash_protocol_selected_log_protocol == "CDBG")
        {
            cdbg_start_logging();
        }
        else
        {
            log_ssm_values();
            logparams_poll_timer->start();
        }
```
with:
```cpp
        logging_counter = 0;
        logging_state = true;

        LogSessionConfig config;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA") {
            config.protocolId = "MUT_DMA";
            config.logValueProtocolFilter = "MUT_DMA";
        } else if (configValues->flash_protocol_selected_log_protocol == "CDBG") {
            config.protocolId = "CDBG";
            config.logValueProtocolFilter = "CDBG";
        } else {
            config.protocolId = "SSM";
            config.logValueProtocolFilter = protocol;
        }
        activeLogValueProtocolFilter = config.logValueProtocolFilter;

        if (!loggingEngine->start(config)) {
            QMessageBox::information(this, tr("Logging"), "Unable to start logging");
            logging_state = false;
            logger->setChecked(false);
            return;
        }
```

- [ ] **Step 2: Replace the stop branch in `menu_actions.cpp::toggle_realtime()`**

Replace (lines 946-961):
```cpp
        logging_state = false;
        log_params_request_started = false;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_stop_logging();
        }
        else if (configValues->flash_protocol_selected_log_protocol == "CDBG")
        {
            cdbg_stop_logging();
        }
        else
        {
            log_ssm_values();
            delay(200);
            logparams_poll_timer->stop();
        }
```
with:
```cpp
        logging_state = false;
        log_params_request_started = false;
        loggingEngine->stop();
```

- [ ] **Step 3: Replace the destructor's logging cleanup in `mainwindow.cpp`**

Replace (`mainwindow.cpp:490-509`):
```cpp
MainWindow::~MainWindow()
{
    if (logging_state)
    {
        logging_state = false;
        log_params_request_started = false;
        if (configValues->flash_protocol_selected_log_protocol == "MUT_DMA")
        {
            mitsubishi_dma_stop_logging();
        }
        else if (configValues->flash_protocol_selected_log_protocol == "CDBG")
        {
            cdbg_stop_logging();
        }
        else
        {
            log_ssm_values();
            delay(200);
        }
    }
    //ssm_init_poll_timer->stop();
    //serial_poll_timer->stop();
    delete ui;
}
```
with:
```cpp
MainWindow::~MainWindow()
{
    if (logging_state)
    {
        logging_state = false;
        log_params_request_started = false;
        loggingEngine->stop();
    }
    delete ui;
}
```

- [ ] **Step 4: Build the app and manually sanity-check the toggle path compiles**

Run: `qmake FastECU.pro && make`
Expected: builds cleanly. `mitsubishi_dma_start_logging`, `mitsubishi_dma_stop_logging`, `cdbg_start_logging`, `cdbg_stop_logging`, `log_ssm_values`, `logparams_poll_timer`, `logging_poll_timer` are now unused by `toggle_realtime()`/the destructor but still exist (Task 9 removes them) -- expect unused-function warnings, not errors, at this step.

- [ ] **Step 5: Commit**

```bash
git add menu_actions.cpp mainwindow.cpp
git commit -m "feat: route toggle_realtime and shutdown cleanup through LoggingEngine"
```

---

## Task 9: Remove the old per-protocol logging code

Deletes the now-unused GUI-thread logging implementations and their timer plumbing. This also eliminates a latent bug: `logparams_poll_timer`'s `timeout()` was connected to `read_log_serial_data()` unconditionally at startup (`mainwindow.cpp:438`) and then *also* connected to `mitsubishi_dma_poll()`/`cdbg_poll()` when those sessions started (without ever disconnecting the original binding), so a MUT/DMA or Cdbg session could have `read_log_serial_data()` firing on the same timer tick and racing for bytes on the same `SerialPortActions` connection. The new architecture runs exactly one `LoggingProtocol` per session on its own thread, so this race cannot recur. It also fixes a related gap: `mitsubishi_dma_poll()`/`cdbg_poll()` never called `update_logbox_values()`, so MUT/DMA and Cdbg sessions never refreshed the on-screen gauges even though `logValues->log_value` was being updated -- `handleLoggingValuesUpdated` (Task 7) now does this uniformly for all three protocols.

**Files:**
- Delete: `log_operations_mitsubishi.cpp`
- Delete: `log_operations_mitsubishi_cdbg.cpp`
- Modify: `log_operations_ssm.cpp` (remove `log_ssm_values()`, `read_log_serial_data()`, `parse_log_params()`)
- Modify: `mainwindow.h` (remove now-dead declarations/members)
- Modify: `mainwindow.cpp` (remove `logging_poll_timer`/`logparams_poll_timer` construction; remove their `->stop()` call sites)
- Modify: `menu_actions.cpp` (remove remaining `logging_poll_timer->stop()` call sites)
- Modify: `FastECU.pro` (remove the two deleted `.cpp` files from `SOURCES`)

**Interfaces:**
- Consumes: nothing new -- this task only removes code made dead by Tasks 7-8.

- [ ] **Step 1: Delete the two fully-replaced files**

```bash
git rm log_operations_mitsubishi.cpp log_operations_mitsubishi_cdbg.cpp
```

- [ ] **Step 2: Remove the ported functions from `log_operations_ssm.cpp`**

Delete `log_ssm_values()` (lines 291-347), `read_log_serial_data()` (lines 349-393), and `parse_log_params()` (lines 395-507) in their entirety. Leave `ssm_init()`, `ssm_kline_init()`, `ssm_can_init()`, `parse_log_value_list()`, `add_ssm_header()`, `calculate_checksum()`, and `log_to_file()` untouched -- they are still used by ECU-connect-time code, not the per-cycle logging loop this refactor replaces.

- [ ] **Step 3: Remove dead declarations and members from `mainwindow.h`**

Remove from the method declaration block:
```cpp
    void mitsubishi_dma_start_logging();
    void mitsubishi_dma_stop_logging();
    void cdbg_start_logging();
    void cdbg_stop_logging();
    QString parse_log_params(QByteArray received, QString protocol);
```
(Leave `bool mut_write_memory(...)` and `QByteArray mut_read_memory(...)` -- those are unrelated bench utilities, not part of the per-cycle logging path, and are not touched by this refactor.)

Remove from `private slots:`:
```cpp
    void log_ssm_values();
    void read_log_serial_data();
    void mitsubishi_dma_poll();
    void cdbg_poll();
```

Remove the now-unused members:
```cpp
    QTimer *logging_poll_timer;
    uint16_t logging_poll_timer_timeout = 150;
    uint16_t logging_counter = 0;
    bool logging_request_active = false;

    QTimer *logparams_poll_timer;
    uint16_t logparams_poll_timer_timeout = 10;
    bool logparams_read_active = false;

    mutdma::MutDmaDriver* mut_driver = nullptr;
    mutdma::FastEcuKlineTransport* mut_transport = nullptr;
    mutdma::IMutDmaInit* mut_init = nullptr;

    MitsuColtCanCdbg::CdbgLogDriver* cdbg_driver = nullptr;
    cdbg::FastEcuCanTransport* cdbg_transport = nullptr;
```

Keep `logging_state`, `log_speed_timer` is now unused too (only referenced from the just-deleted `parse_log_params()`) -- remove `QElapsedTimer *log_speed_timer;` as well, and its `mainwindow.cpp:440` construction (`log_speed_timer = new QElapsedTimer();`) in the next step. Double check with `grep -rn "log_speed_timer" *.cpp *.h` that no other reference remains before deleting; if one does, leave the member in place and note it in this task's completion notes instead of deleting.

- [ ] **Step 4: Remove the timer construction in `mainwindow.cpp`**

Remove (lines 432-441):
```cpp
    logging_poll_timer = new QTimer(this);
    logging_poll_timer->setInterval(logging_poll_timer_timeout);
    connect(logging_poll_timer, SIGNAL(timeout()), this, SLOT(log_ssm_values()));

    logparams_poll_timer = new QTimer(this);
    logparams_poll_timer->setInterval(logparams_poll_timer_timeout);
    connect(logparams_poll_timer, SIGNAL(timeout()), this, SLOT(read_log_serial_data()));

    log_speed_timer = new QElapsedTimer();
    log_file_timer = new QElapsedTimer();
```
with (keeping the unrelated `log_file_timer`):
```cpp
    log_file_timer = new QElapsedTimer();
```

Remove the two remaining `logging_poll_timer->stop();` call sites in `mainwindow.cpp` (in `can_listener()` and `start_ecu_operations()`, identified via `grep -n "logging_poll_timer" mainwindow.cpp`).

- [ ] **Step 5: Remove the remaining `logging_poll_timer->stop();` call sites in `menu_actions.cpp`**

Run `grep -n "logging_poll_timer" menu_actions.cpp` and delete each matching line (three call sites per the earlier repo-wide grep, at approximately lines 1101, 1486, 1672 -- confirm exact line numbers before editing, since Task 8's edits shift some line numbers in this file).

- [ ] **Step 6: Remove the deleted files from `FastECU.pro`**

Remove `log_operations_mitsubishi.cpp` and `log_operations_mitsubishi_cdbg.cpp` from the `SOURCES` list.

- [ ] **Step 7: Build the full app and the test suite**

Run: `qmake FastECU.pro && make`
Expected: builds cleanly with no references to the removed symbols.

Run: `cd tests && qmake tests.pro && make && ./mut_dma_tests`
Expected: all suites from Tasks 1-6 still pass (this task does not touch anything the unit tests exercise directly, but confirms nothing else in `tests.pro` referenced the removed files).

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor: remove GUI-thread logging code superseded by LoggingEngine"
```

---

## Task 10: Bench verification checklist

No CI coverage is realistic for the end-to-end behavior (real J2534/serial hardware and a live ECU are required), matching the existing precedent in `docs/cdbg-can-logging-bench-checklist.md` and `m32r/39670016_z27a_mt_audm/mode23-bench-notes.md` (in the sibling `mmc-patches` repo). This task adds the equivalent checklist for this refactor.

**Files:**
- Create: `docs/logging-engine-bench-checklist.md`

**Interfaces:**
- Consumes: nothing (documentation only).

- [ ] **Step 1: Write `docs/logging-engine-bench-checklist.md`**

```markdown
# Logging engine refactor -- bench verification checklist

Verifies the LoggingEngine/LoggingWorker/LoggingProtocol refactor (see
`docs/superpowers/specs/2026-07-02-logging-engine-refactor-design.md`) behaves
correctly against real hardware. The unit test suite (`tests/mut_dma_tests`)
covers the worker/engine/protocol logic against scripted transports; this
checklist covers what can only be observed with a real adapter and ECU.

## Per protocol (repeat for SSM, MUT/DMA, Cdbg)

1. Start logging from the GUI. Confirm gauge values update continuously and the
   GUI remains responsive (no beachball/freeze) while logging is active --
   the core regression this refactor targets.
2. Stop logging from the GUI. Confirm the session ends within roughly one
   poll cycle (SSM: ~300ms, MUT/DMA and Cdbg: ~50ms) of clicking Stop, and the
   "Logging" menu item unchecks itself.
3. Unplug the adapter (USB/OpenPort 2.0) while logging is active. Confirm a
   "Logging adapter disconnected" message appears and the session stops
   cleanly (no crash, no hang).
4. Turn the ignition off (or otherwise make the ECU stop responding) while
   the adapter stays connected. Confirm logging keeps running (gauges may
   freeze/show stale values) rather than stopping outright, and that turning
   the ignition back on resumes live values without restarting logging from
   the GUI.
5. Confirm values displayed match the equivalent pre-refactor build for the
   same channels (semantic parity check, not byte-exact -- per this project's
   established port-verification convention).

## Regression checks

6. Start a MUT/DMA or Cdbg session (previously never called
   `update_logbox_values()` -- see Task 9's completion notes) and confirm the
   lower-panel gauges actually refresh, not just `logValues->log_value` in
   memory.
7. Confirm no other serial operation (ROM read/flash) can be started while a
   logging session is active, matching the pre-refactor `logging_state` guard.
```

- [ ] **Step 2: Commit**

```bash
git add docs/logging-engine-bench-checklist.md
git commit -m "docs: add bench verification checklist for the logging engine refactor"
```
