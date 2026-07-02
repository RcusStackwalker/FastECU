# Serial backend decoupling — design

**Date:** 2026-07-02
**Status:** Approved for planning
**Predecessor:** `2026-07-02-logging-engine-refactor-design.md` (established the
interface/adapter/scripted-double pattern this design extends; its tech-debt doc
`docs/logging-engine-tech-debt.md` names the two architectural fixes this spec and its
successors deliver)

## Context and problem

`SerialPortActions` is the single gateway to the vehicle adapter (OpenPort 2.0/J2534,
plain `QSerialPort`, or a QtRemoteObjects/WebSocket remote peer). It has three structural
problems, all confirmed during the logging engine refactor:

1. **Wrong thread affinity.** `SerialPortActionsDirect` creates its `QSerialPort` with
   GUI-thread affinity, but `LoggingWorker` now calls `poll()` from its own thread.
   Cross-thread `QSerialPort` I/O may silently fail or warn ("socket notifiers used from
   another thread"). This is item 1 on `docs/logging-engine-bench-checklist.md`.
2. **`QCoreApplication::processEvents()` pumping inside blocking I/O.** Blocking reads pump
   the caller's event queue to fake asynchrony. On the GUI thread this is what keeps the UI
   alive during multi-minute flash operations (i.e. the pump is load-bearing), but it also
   allows arbitrary reentrancy: a timer or user action can re-enter serial I/O mid-read.
3. **Untestable as a unit.** There is no interface seam around the backend; the test
   binaries must link the full facade including QtRemoteObjects and QtWebSockets, and the
   routing/timeout/buffer logic can only be exercised against real hardware.

## Scope decisions (from brainstorming)

- **This is the first of three chained specs.** This spec (1a) restructures the serial
  backend. Spec 1b (successor, not designed here) moves the ~30 flash/eeprom modules off
  the GUI thread onto a worker — they are `QDialog` subclasses whose only UI use is
  `setupUi` + progress-bar updates, with an `external_logger(int)` signal path already in
  place — and deletes the GUI compat shim introduced here. A separate spec covers splitting
  `FileActions`' pure logic (expression evaluation, config/definition parsing) out of its
  `QWidget` base.
- **Target test levels:** backend/adapter level (routing, timeouts, buffer handling against
  fakes) and thread-safety level (facade driven from non-GUI threads). Flash-module-vs-
  fake-ECU testing and headless app-level testing are explicitly out of scope here.
- **Threading model:** dedicated I/O thread owning the port objects; blocking facade API
  preserved; no `processEvents()` in the backend.
- **Remote path:** preserved and placed behind the same backend interface; remote behavior
  itself unchanged.
- **GUI-thread callers:** keep today's UX via a temporary, clearly-marked compat shim (see
  Threading model); deleted by spec 1b.
- **Public API:** `SerialPortActions`' ~100-method public surface is kept as-is. Narrowing
  it is out of scope; no consumer (flash modules, `dtc_operations`, `ecu_operations`,
  `dataterminal`, `hexcommander`, logging transports, `MainWindow`) changes in this spec.

## Architecture

```
                       consumers (unchanged)
  flash modules, ecu/dtc_operations, logging transports, MainWindow
                              │ blocking calls (public API unchanged)
                              ▼
                    SerialPortActions (facade)
                    marshals each call to the backend's thread,
                    blocks caller until completion
                              │
              ┌───────────────┴───────────────┐
              ▼                               ▼
     DirectSerialBackend              RemoteSerialBackend
     (on SerialIoThread)              (QtRemoteObjects replica +
     owns QSerialPort + J2534         WebSocket; behavior unchanged)
     no processEvents() anywhere
```

### Components

- **`SerialBackend`** (new, internal abstract interface): the I/O contract — open/close,
  read/write, port settings, protocol flags, J2534 ioctls, five-baud/fast init, periodic
  messages. Internal only: consumers never see it; it exists so the facade, the two
  backends, and the tests share one seam.
- **`DirectSerialBackend`**: today's `SerialPortActionsDirect` logic with every
  `processEvents()` call removed. Owns the `QSerialPort` and the `J2534` wrapper. Lives on
  `SerialIoThread`; the `QSerialPort` is *created on* that thread so socket-notifier
  affinity is correct by construction. Blocking waits inside the backend use
  `waitForReadyRead()`/timers locally — legal because nothing else runs on that thread.
  The existing protected test seams (`J2534 *j2534`, `QSerialPort *serial`) are preserved.
- **`SerialIoThread`** (new): a `QThread` running an event loop, owning the
  `DirectSerialBackend`. All direct-backend calls execute here. This is the single
  serialization point: concurrent callers queue rather than interleave. The
  `is_comm_busy` flag remains as the user-facing busy indicator, but the queue is the
  actual mutual-exclusion mechanism.
- **`RemoteSerialBackend`**: wraps the existing replica/WebSocket path behind
  `SerialBackend`. No wire-behavior change; the facade's direct-vs-remote routing decision
  becomes a testable seam. It is hosted on `SerialIoThread` like the direct backend, so
  both paths share the same serialization point and neither has GUI-thread affinity.
- **`SerialPortActions`** (existing class, rewritten internals): thin marshaling facade.
  Public surface unchanged so all consumers compile untouched.

## Threading model and data flow

- **Non-GUI-thread caller** (LoggingWorker, tests, future operation workers): the facade
  posts the command to the backend thread and blocks on a condition/semaphore until the
  result is ready. No event pumping, no affinity warnings, no reentrancy.
- **GUI-thread caller** (flash modules until spec 1b lands): the facade posts the command,
  then pumps `processEvents()` in its wait loop so repaints and progress bars stay alive.
  This is the **compat shim**: one clearly-marked branch
  (`if caller is GUI thread → pump-wait, else → condition-wait`) with a comment naming
  spec 1b as its deletion point. Reentrancy from the pump is bounded by the I/O-thread
  queue: a re-entered facade call queues behind the in-flight one instead of interleaving
  reads on the wire (today's latent corruption risk).
- **Signals:** backend `LOG_*` and state signals become cross-thread queued connections —
  native Qt behavior, no code changes at the receivers.
- **Error handling:** facade-level semantics are unchanged — same return types, same
  timeout behavior, same empty-`QByteArray`/error-code conventions. Consumers cannot tell
  the difference except that affinity warnings disappear.
- **Lifecycle:** `SerialIoThread` starts lazily on first open and is joined on facade
  destruction; open/close/reset marshal like any other call so teardown ordering is
  serialized with in-flight I/O (the crash class `tst_serial_port_crash.cpp` exercises).

## Testing

**Backend/adapter level** (new):
- `DirectSerialBackend` against a **fake `J2534` subclass** (protected seam already exists,
  used by the crash tests): connect/disconnect routing, read timeout expiry, partial and
  fragmented message reassembly, buffer clearing, ioctl parameter handling, error-path
  behavior when the adapter vanishes mid-operation.
- **pty-backed `QSerialPort` test** (unix-only, skipped elsewhere): the plain-serial path
  end-to-end through a pseudo-terminal — open, write/echo, timed read — proving the
  no-`processEvents()` backend actually delivers data on the I/O thread.

**Thread-safety level** (new):
- Drive the facade from a spawned worker thread (the exact LoggingWorker scenario from the
  bench checklist): data delivered, no Qt affinity warnings emitted (test hooks
  `qInstallMessageHandler` and fails on them).
- Concurrent callers from two threads: calls serialize; no interleaved wire traffic
  (observable through the fake J2534's call log).
- GUI-thread caller with the compat shim: a `QTimer`-driven repaint counter proves the
  event loop stays live during a long blocking read.

**Regression net** (existing, must stay green): `tests/tests.pro` unit suite (logging
protocols, engine, worker), `serial_crash_tests.pro`, `mut_dma_integration_tests.pro`.

**Bench gate:** existing checklists; `docs/logging-engine-bench-checklist.md` item 1
(direct-serial under the LoggingWorker threading model) is directly resolved by this
design and should be re-verified on hardware after it lands.

## Out of scope

- Narrowing/splitting the `SerialPortActions` public API surface.
- Moving flash/ecu/dtc operations off the GUI thread (spec 1b).
- `FileActions` QWidget split (separate spec).
- Any change to remote-mode wire behavior or the `.rep` interface.
- Consumer-level (flash-module-vs-fake-ECU) or headless app-level test infrastructure.

## Risks

- **Behavioral drift in timeout semantics.** The pump-based waits had fuzzy timing; the
  condition-wait path is stricter. Mitigation: backend tests pin the timeout contract
  before the facade is rewired; bench verification is the final gate.
- **Hidden reliance on pump reentrancy.** Some GUI code may depend on re-entering serial
  I/O from within a blocking call (e.g. user clicks during flash). The queue makes this
  safe but changes ordering. Mitigation: thread-safety tests cover the re-entrant case;
  bench checklist covers real-user flows.
- **Remote path regression.** `RemoteSerialBackend` is a mechanical wrap, but remote mode
  has no automated tests. Mitigation: keep the wrap strictly mechanical; manual smoke test
  of remote mode before release.
