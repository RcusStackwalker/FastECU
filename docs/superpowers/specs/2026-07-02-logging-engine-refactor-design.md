# Decouple and generalize FastECU's live-data logging engine

## Goal

FastECU currently implements three live-data logging protocols — SSM (Subaru, request/response polling), MUT/DMA (Mitsubishi K-Line, setup-and-listen streaming), and Cdbg (Mitsubishi CAN, setup-and-listen streaming) — each as a set of `MainWindow::` methods (`log_operations_ssm.cpp`, `log_operations_mitsubishi.cpp`, `log_operations_mitsubishi_cdbg.cpp`) driven by `QTimer`s on the GUI thread, with blocking serial I/O calls executed inline. Adding or evolving a protocol means editing `MainWindow` directly, and any blocking I/O stalls the GUI (mitigated today only by `SerialPortActions::read_serial_data()` pumping `QCoreApplication::processEvents()` while it waits — an accidental "fake async" that only works because it happens to run on the GUI thread).

This spec replaces that with a small logging engine that: runs entirely off the GUI thread; expresses "polling" and "setup-and-listen" protocols through one interface without imposing an artificial cadence on either; reacts promptly to a stop request; and distinguishes adapter-level connectivity loss from ECU/car silence, handling each appropriately.

## Current state (for reference)

- `MainWindow` holds `logValues` (a `FileActions::LogValuesStructure*` — plain, unsynchronized parallel `QStringList`s, notably `log_value_id`, `log_value_address`, `log_value_protocol`, `log_value_enabled`, `log_value_units`, `log_value_length`, `log_value`). Both the logging code and the GUI (`update_logboxes()`, `update_logbox_values()` in `mainwindow.cpp`) read/write it directly, with no locking, because everything happens on the GUI thread today.
- `menu_actions.cpp::toggle_realtime()` branches on `configValues->flash_protocol_selected_log_protocol` ("MUT_DMA" / "CDBG" / else-SSM) to call the matching pair of `*_start_logging()` / `*_stop_logging()` methods. `MainWindow::~MainWindow()` has an identical branch for shutdown cleanup.
- `SerialPortActions` (`serial_port/serial_port_actions.h`) is a facade over a direct hardware backend (`QSerialPort` or J2534/OpenPort 2.0) or a remote-replica backend. `read_serial_data()`/`write_serial_data()` are synchronous, timeout-bounded calls. There is no unified connected/disconnected signal — only a remote-mode `stateChanged`, a J2534-specific `reportJ2534Error()`, and a `QSerialPort` error slot. It has never been called from more than one thread.
- The only existing multi-threading precedent is `SystemLogger`: a `QObject` moved to a `QThread` via `moveToThread()`, driven by the thread's Qt event loop, communicating exclusively through queued signals/slots. This fits a "drain a queue of log lines" job but not a "continuous back-to-back blocking I/O" job — see Architecture below for why this design departs from it.
- `MutDmaDriver` (`protocol/mut_dma_freeform.h`) and `CdbgLogDriver` (`protocol/mitsu_colt_can_cdbg_protocol.h`) already expose a `startFreeFormLog(...)` / `pollOnce(timeoutMs)` / `isStreaming()` shape, which this design generalizes into a common protocol interface.
- `tests/` already has scripted transport fixtures (`scripted_kline_transport.h`, `scripted_can_transport.h`) used to unit-test `MutDmaDriver`/`CdbgLogDriver` without hardware — the precedent this design's tests follow.

## Architecture

### `LoggingProtocol` interface

```cpp
class LoggingProtocol {
public:
    virtual ~LoggingProtocol() = default;
    // Handshake / session setup. Returns false and sets *errorOut on failure.
    virtual bool start(QString *errorOut) = 0;
    // Blocks until the next result is available or timeoutMs elapses, then returns.
    // Polling protocols (SSM): sends the next request, blocks for the response.
    // Setup-and-listen protocols (MUT/DMA, CDBG): blocks until the next pushed frame arrives.
    virtual PollResult poll(int timeoutMs) = 0;
    virtual void stop() = 0;
};

struct LogSample { int logValueIndex; QString displayValue; };  // already expression-converted, display-ready

struct PollResult {
    enum class Status { Ok, NoResponse, TransportError };
    Status status;
    QVector<LogSample> samples;   // valid when status == Ok; already display-ready (see below)
    QString errorMessage;         // valid when status == TransportError
};
```

Three concrete implementations port the existing per-protocol logic:
- `SsmLoggingProtocol` — ports the request-building and response-parsing logic from `log_operations_ssm.cpp` (`log_ssm_values()`, `read_log_serial_data()`, `parse_log_params()`) into `start()`/`poll()`.
- `MutDmaLoggingProtocol` — thin adapter over the existing `MutDmaDriver`, porting `mitsubishi_dma_start_logging()`/`mitsubishi_dma_poll()`/`mitsubishi_dma_stop_logging()` from `log_operations_mitsubishi.cpp`.
- `CdbgLoggingProtocol` — thin adapter over the existing `CdbgLogDriver`, porting `cdbg_start_logging()`/`cdbg_poll()`/`cdbg_stop_logging()` from `log_operations_mitsubishi_cdbg.cpp`.

Each implementation also owns the RomRaider expression conversion (the `log_value_units` split into from-byte expression + format, currently duplicated near-identically in `mitsubishi_dma_poll()` and `cdbg_poll()`) so that `poll()`'s `Ok` result carries display-ready values, not raw bytes.

Adding a future protocol means writing one new `LoggingProtocol` implementation and one registry entry (see `LoggingEngine` below) — no changes to the worker or engine.

### `LoggingWorker` (the thread)

A `QThread` subclass (not a `moveToThread()`'d `QObject` driven by a `QTimer`, unlike `SystemLogger`) because the requirement is that a polling protocol issues its next request immediately upon receiving the previous response, and a streaming protocol processes each frame as soon as it arrives — no timer-imposed cadence. `run()`:

```cpp
void LoggingWorker::run() {
    QString err;
    if (!m_protocol->start(&err)) {
        emit sessionEnded(SessionEndReason::HandshakeFailed, err);
        return;
    }
    emit statusChanged(LoggingStatus::Running);

    int consecutiveMisses = 0;
    while (!m_stopRequested.loadRelaxed()) {
        PollResult r = m_protocol->poll(kPollTimeoutMs);
        switch (r.status) {
        case PollResult::Status::Ok:
            consecutiveMisses = 0;
            if (m_lastStatus != LoggingStatus::Running) {
                m_lastStatus = LoggingStatus::Running;
                emit statusChanged(LoggingStatus::Running);
            }
            emit valuesUpdated(r.samples);  // already LogSample, no conversion needed here
            break;
        case PollResult::Status::NoResponse:
            if (++consecutiveMisses == kCarSilenceMissThreshold) {
                m_lastStatus = LoggingStatus::CarNotResponding;
                emit statusChanged(LoggingStatus::CarNotResponding);
            }
            if (consecutiveMisses >= kReconnectAttemptThreshold
                && (consecutiveMisses - kReconnectAttemptThreshold) % kReconnectRetryPeriod == 0) {
                if (m_protocol->start(&err)) {
                    consecutiveMisses = 0;
                    m_lastStatus = LoggingStatus::Running;
                    emit statusChanged(LoggingStatus::Running);
                }
            }
            break;
        case PollResult::Status::TransportError:
            m_protocol->stop();
            emit sessionEnded(SessionEndReason::AdapterDisconnected, r.errorMessage);
            return;
        }
    }
    m_protocol->stop();
    emit sessionEnded(SessionEndReason::StoppedByUser, QString());
}

void LoggingWorker::stop() { m_stopRequested.storeRelaxed(true); }
```

`m_stopRequested` is a plain atomic flag, settable directly from the GUI thread without needing the worker to run its own event loop — `stop()` does not need to be a queued slot. Because `kPollTimeoutMs` bounds every blocking call, the loop notices a stop request within one poll cycle (same order of magnitude as today's 10ms `logparams_poll_timer`). `valuesUpdated`/`statusChanged`/`sessionEnded` are ordinary Qt signals; delivering them to GUI-thread slots as queued calls only requires the *receiving* thread (the GUI thread) to run an event loop, which it always does — the worker thread emitting them does not need one.

`kCarSilenceMissThreshold`, `kReconnectAttemptThreshold`, and `kReconnectRetryPeriod` are per-protocol tunables (SSM's, MUT/DMA's, and CDBG's natural cycle times differ), passed into the worker alongside the protocol instance.

### `LoggingEngine` (GUI-thread façade)

Owns the current session's `LoggingWorker*` (if any). Exposes:
- `bool start(const LogSessionConfig &config)` — looks up the `LoggingProtocol` factory for `config.protocolId` in a static registry (`QMap<QString, std::function<std::unique_ptr<LoggingProtocol>(const LogSessionConfig&)>>`), constructs the protocol and worker, connects the worker's signals to the engine's own (re-emitted for `MainWindow` to consume), and calls `QThread::start()`.
- `void stop()` — calls the worker's `stop()`, then `QThread::wait()` (bounded) before tearing down.
- Signals: `valuesUpdated(QVector<LogSample>)`, `statusChanged(LoggingStatus)`, `sessionEnded(SessionEndReason, QString message)`.

`LogSample = { int logValueIndex; QString displayValue; }`.

## Connectivity & error handling

Two distinct failure classes, matching how they differ operationally:

- **Car/ECU silence** (ignition off, ECU busy, momentary bus noise) is *not* fatal. The loop keeps polling/re-handshaking indefinitely. After `kCarSilenceMissThreshold` consecutive `NoResponse` results, the engine reports `LoggingStatus::CarNotResponding` so the GUI can show a transient indicator, but the session stays alive. Past `kReconnectAttemptThreshold`, the worker periodically re-runs `protocol->start()` (a fresh handshake) rather than bare `poll()`, since an ECU reset invalidates any prior streaming session state for MUT/DMA and CDBG. The instant a poll or re-handshake succeeds, status flips back to `Running` with no user action.
- **Adapter loss** (USB unplugged, J2534 handle lost, serial port hardware error) is fatal to the session: any `PollResult::TransportError` ends the loop immediately, calls `protocol->stop()`, and emits `sessionEnded(AdapterDisconnected, message)` — a status distinct from car silence, so the GUI can tell the user to check hardware rather than wait.
- **Handshake failure on initial `start()`** ends the session immediately with `sessionEnded(HandshakeFailed, message)`, mirroring the current `emit LOG_E(...)` + cleanup path in the three existing `*_start_logging()` functions.
- **User-requested stop** exits the loop cleanly via the atomic flag and emits `sessionEnded(StoppedByUser, {})`.

## GUI integration

- Each `LoggingProtocol` implementation computes display-ready values itself, so `MainWindow` only needs one new GUI-thread slot (replacing the current inline call to `update_logbox_values()` from inside `parse_log_params()`) that writes each `LogSample` into `logValues->log_value` and updates the corresponding `QLabel`. `LogValuesStructure` is touched from both threads: the worker thread reads the configuration/metadata fields (`log_value_id`, `log_value_protocol`, `log_value_address`, `log_value_length`, `log_value_enabled`, `log_value_units`, `lower_panel_log_value_id`) inside `SsmLoggingProtocol::poll()` and the channel-building code in `MutDmaLoggingProtocol::start()`/`CdbgLoggingProtocol::start()`, while the GUI thread writes `log_value` via `handleLoggingValuesUpdated`. No locking is introduced; this is safe without it only because (a) the worker only ever reads the configuration/metadata lists, never the `log_value` list the GUI writes, and (b) per-session configuration is fixed for the session's lifetime (see "no live reconfiguration during a session" in Scope below), so the lists the worker reads don't change while a session is active. If that invariant is ever relaxed, this would need real synchronization.
- `menu_actions.cpp::toggle_realtime()` and `MainWindow::~MainWindow()` collapse their three-way protocol branches into `loggingEngine->start(config)` / `loggingEngine->stop()`; `LoggingEngine` picks the right protocol internally via its registry.
- The `logging_poll_timer` and `logparams_poll_timer` `QTimer`s, and the `MainWindow::` methods in the three `log_operations_*.cpp` files, are removed — their logic moves into the three `LoggingProtocol` implementations.
- `LogBox`/`LogValues` (gauge drawing/layout) are unchanged; they already only render whatever is in `logValues`.
- `SerialPortActions` itself is not redesigned — same synchronous, timeout-bounded API — but during an active session it is called exclusively from the `LoggingWorker` thread. The existing invariant that no other serial operation (ROM read/flash) runs concurrently with logging (today enforced via the `logging_state` guard) is preserved, now backed by the worker thread's exclusive ownership of `SerialPortActions` for the session's duration.

## Scope

All three existing protocols (SSM, MUT/DMA, CDBG) are ported to the new engine in this effort — none is left on the old GUI-thread path. Per-session configuration (which channels, protocol choice) is fixed for the session's lifetime; changing it means stop-then-start, matching how `menu_actions.cpp` already treats logging as an atomic on/off toggle today. Live reconfiguration (channel list or rate changes without a stop) is explicitly out of scope. `SerialPortActions`'s internal blocking-I/O implementation is not redesigned.

## Testing

No real hardware or emulator, consistent with this project's existing convention and the scripted-transport pattern already used for `MutDmaDriver`/`CdbgLogDriver`:

- **Protocol-level:** extend the existing scripted transport fixtures (`scripted_kline_transport.h`, `scripted_can_transport.h`) to also exercise `SsmLoggingProtocol`'s request/response logic.
- **Worker-level** (`QSignalSpy` + `QTest::qWaitFor` against a `LoggingWorker` driven by a scripted `LoggingProtocol` test double):
  - Normal run: `statusChanged(Running)` followed by a stream of `valuesUpdated` carrying correctly converted values.
  - Handshake failure: `sessionEnded(HandshakeFailed, ...)` with the loop never entering the poll phase.
  - Car dropout then recovery: enough scripted `NoResponse` results to cross `kCarSilenceMissThreshold` produces `statusChanged(CarNotResponding)`; a subsequent scripted success produces `statusChanged(Running)` again.
  - Adapter loss: a scripted `TransportError` produces `sessionEnded(AdapterDisconnected, ...)` and the thread actually exits (`QThread::wait()` returns promptly).
  - Stop responsiveness: calling `stop()` while the scripted protocol is mid-`poll()` (with an injected delay bounded by the timeout) results in the thread exiting within one poll-cycle bound.
