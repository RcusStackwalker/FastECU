# Flash operation worker migration — design

**Date:** 2026-07-03
**Status:** Landed — pending bench re-verification per module (see docs/logging-engine-tech-debt.md)
**Predecessor:** `2026-07-02-serial-backend-decoupling-design.md` ("spec 1a" — restructured
`SerialPortActions` into a thin marshaling facade over a dedicated I/O thread; named this
work "spec 1b" as its successor and the compat shim's deletion point)

## Context and problem

Spec 1a made `SerialPortActions` safe to call from any thread, but kept a GUI-thread compat
shim in `waitForDone()` (`serial_port_actions.cpp`) because the ~30 flash/eeprom modules
under `modules/` still call it synchronously from the GUI thread during multi-minute
read/write operations. The shim pumps `QCoreApplication::processEvents()` in the caller's
wait loop so the UI stays alive — a direct carry-over of the pre-1a "fake async" pattern,
now confined to one call site instead of scattered through the backend.

Inspection of the 30 modules (all `QDialog` subclasses under `modules/bdm`, `modules/biu`,
`modules/bootmode`, `modules/ecu`, `modules/eeprom`, `modules/jtag`, `modules/tcu`) found
three compounding problems, confirmed uniformly across the set (29-31 of 30 files match each
pattern):

1. **`run()` interleaves blocking I/O with GUI-only calls in one function.** Every module's
   `run()` mixes `serial->write_serial_data_echo_check()`/`read_serial_data()` with
   `QMessageBox::warning()`/`information()` (8-17 occurrences per module), `this->show()`,
   and `this->close()`. Some `QMessageBox` prompts are pre-flight (before any I/O), but at
   least one per module is a genuine **mid-operation** gate — e.g.
   `FlashEcuMitsuM32rCan::write_mem()`'s "about to send the flash-erase trigger, this is known
   to have locked up the bootloader during testing, continue?" prompt fires only after
   `connect_bootloader()` and two `upload_and_commit()` calls have already succeeded.
2. **Two independent `processEvents()` pump sites, not just the facade's shim.**
   `set_progressbar_value()` touches `ui->progressbar` directly and pumps events itself;
   `delay()` busy-loops on `processEvents()` for its sleep. Both are load-bearing today (they
   keep the UI alive during the tight blocking loop) and both need to go if the operation
   moves off the GUI thread.
3. **`kill_process` is a plain, non-atomic `bool`.** Set in `closeEvent()`, read in the
   read/write loop. Harmless today because both sides run on the GUI thread (the pump makes
   `closeEvent()` a nested reentrant call within the blocking loop), but a genuine data race
   once the loop runs on a separate thread.

A fourth, non-obvious constraint shapes the whole design: `MainWindow::start_ecu_operations()`
stack-allocates each flash module (`FlashEcuMitsuM32rCan flash_module(serial, ...);`) at each
of its ~30 dispatch call sites, calls `object->run()` through the
`connect_signals_and_run_module()` template, and **synchronously continues** past that call —
reading `ecuCalDef[ecuCalDefIndex]->FullRomData` back out, building calibration tree widgets,
saving the calibration file, stopping `vbatt_timer`, and calling `serial->reset_connection()`.
This code assumes `run()` doesn't return until the operation is fully done. A fire-and-forget
async redesign (`run()` returns immediately, completion signaled later — the shape used by
`LoggingEngine`/`LoggingWorker`) would destroy the stack-allocated dialog while its worker
thread was still running, and would require moving all of that post-`run()` logic into a
completion callback at every one of the 30 call sites.

## Scope decisions (from brainstorming)

- **Shim deletion scope:** the compat shim is deleted outright once the 30 flash/eeprom
  modules no longer need it. `dtc_operations`, `dataterminal`, and `hexcommander` also call
  `SerialPortActions` from the GUI thread, but with short, click-bounded calls
  (`serial_read_short_timeout`) rather than multi-minute loops — they were not named in spec
  1a's phase-1b scope line and are explicitly **not** migrated here. After shim deletion,
  their calls fall through to a plain `done->acquire()`: a brief UI freeze (no repaint) per
  click, accepted as a minor regression versus rewriting three more GUI classes that don't
  have the multi-minute-operation problem this spec exists to solve.
- **`run()` keeps its blocking contract.** Rather than redesigning `MainWindow` and its ~30
  call sites around an async completion callback, each module's `run()` still blocks until
  the operation is fully finished — but internally, the blocking wait is a real `QEventLoop`
  serviced by the GUI thread's actual event loop, not a manual `processEvents()` spin. This
  keeps `mainwindow.cpp` (the dispatch chain, the stack allocation, the post-`run()`
  ROM-data/tree-widget/file-save logic) completely unchanged.
- **All 30 modules migrate in this phase**, not a pilot-then-follow-on split. Once the shared
  `FlashOperationWorker` base and the per-module split pattern exist, the remaining modules
  are mechanical repetitions of the same transformation; splitting the rollout would leave the
  codebase running two threading models for flash modules simultaneously for an extended
  period, and the compat shim couldn't be deleted until the last one lands anyway.
- **Logic relocates into new `Operation` classes, not left in-place behind a thread hop.**
  Each module gets a paired `XxxOperation : public FlashOperationWorker` class that owns the
  relocated protocol methods (`connect_bootloader()`, `read_mem()`, `write_mem()`,
  `upload_and_commit()`, `build_request()`, etc.), so worker-thread code has no `ui` member to
  reach for — compiler-enforced separation rather than convention, and each `Operation`
  becomes unit-testable against a fake `SerialBackend` the same way `DirectSerialBackend` is
  tested today (spec 1a's `backendFactoryForTests` seam).
- **No protocol-behavior change.** Every `QMessageBox` prompt, every I/O sequence, every
  timeout, stays byte-for-byte identical — this is a pure threading refactor.

## Architecture

```
                MainWindow::start_ecu_operations()  (unchanged)
                          |  stack-allocates, then:
                          |  connect_signals_and_run_module(&flash_module)
                          |    ...connects LOG_E/W/I/D, external_logger(QString/int)...
                          |    object->run()   <- still BLOCKS, same as today
                          v
                FlashEcuXxx : public QDialog   (existing class, shrunk)
                GUI-thread only: setupUi, show()/close(), QMessageBox,
                ui->progressbar, closeEvent()
                          |  owns, on run():
                          |    creates XxxOperation, connects its signals,
                          |    starts it, then blocks in a QEventLoop
                          |    until operationFinished(bool) arrives
                          v
                XxxOperation : public FlashOperationWorker(QThread)  (new, one per module)
                Worker-thread only: connect_bootloader/read_mem/write_mem/
                upload_and_commit/build_request/... (relocated verbatim from
                the dialog's current private methods) + serial-> calls
                          |
                          v
                SerialPortActions (unchanged public API, per spec 1a)
```

### Components

- **`FlashOperationWorker`** (new, shared base under e.g. `modules/flash_operation_worker.h`,
  mirrors `LoggingWorker`): a `QThread` subclass providing what every module currently
  hand-rolls:
  - `LOG_E/W/I/D(QString,bool,bool)` signals and `progressChanged(int)` (replaces direct
    `ui->progressbar->setValue()`).
  - `operationFinished(bool success)` signal, emitted once at the end of `run()` (mirrors
    `LoggingWorker::sessionEnded`).
  - `QAtomicInteger<bool> m_stopRequested` + `requestStop()` (replaces the plain
    `bool kill_process`, closing the cross-thread race named above).
  - `int confirm(const QString &title, const QString &text, QMessageBox::StandardButtons
    buttons, QMessageBox::StandardButton defaultButton)`: calls
    `QMetaObject::invokeMethod(m_dialog, lambda, Qt::BlockingQueuedConnection, &result)` where
    the lambda calls `QMessageBox::warning(m_dialog, title, text, buttons, defaultButton)` on
    the GUI thread. This lets mid-operation confirmation prompts run on the GUI thread and
    block the worker for the user's answer, with **no new per-dialog code** — no dialog
    implements a slot for this. The prompt callback is injectable (constructor parameter,
    defaults to the real `QMessageBox` lambda) so tests can auto-answer without popping a
    modal.
  - `void run() override` calls a pure-virtual `bool execute()` implemented by each
    `XxxOperation` subclass, then emits `operationFinished(result)` — same delegation shape as
    `LoggingWorker::run()` calling into `LoggingProtocol`.
  - Destructor: `requestStop(); wait();` (identical to `LoggingWorker`'s), so a dialog that
    just deletes its `Operation` on teardown gets safe joining for free.
  - Protected `void delay(int ms)` helper: `QThread::msleep(ms)`. No `processEvents()` —
    nothing else runs on this thread, so there's nothing to pump.

- **`XxxOperation`** (new, one per module, e.g. `FlashEcuMitsuM32rCanOperation`): holds
  exactly what's currently in each dialog's private section — the relocated protocol methods,
  protocol constants (`STATUS_SUCCESS`/`STATUS_ERROR`, timeouts), and `ecuCalDef`/`cmd_type`.
  `set_progressbar_value(v)` becomes `emit progressChanged(v)`; `delay(ms)` becomes the base
  class's `QThread::msleep`. `execute()` is the relocated body of today's `run()`'s
  switch-case (minus `show()`/`close()`/the pre-flight `QMessageBox`, which stay on the
  dialog) — it calls `connect_bootloader()`, then `read_mem()`/`write_mem()`, checking
  `m_stopRequested`/calling `confirm()` at exactly the points `kill_process`/`QMessageBox`
  appear today.

- **`FlashEcuXxx`** (existing, shrunk): keeps `setupUi`, `show()`/`close()`, `closeEvent()`
  (now just `if (m_operation) m_operation->requestStop();` — non-blocking, since the
  `QEventLoop` in `run()` unblocks once the worker notices the flag and emits
  `operationFinished`), the initial pre-flight `QMessageBox` (e.g. "turn ignition ON and press
  OK"), and a `run()` shaped like:

  ```cpp
  void FlashEcuXxx::run()
  {
      this->show();
      // ...pre-flight QMessageBox, unchanged...
      m_operation = new XxxOperation(serial, ecuCalDef, cmd_type, /*dialog=*/this);
      connect(m_operation, &FlashOperationWorker::LOG_E, this, &FlashEcuXxx::LOG_E);
      // ...LOG_W/I/D...
      connect(m_operation, &FlashOperationWorker::progressChanged,
              this, &FlashEcuXxx::set_progressbar_value);
      QEventLoop loop;
      bool success = false;
      connect(m_operation, &FlashOperationWorker::operationFinished, &loop,
              [&](bool ok) { success = ok; loop.quit(); });
      m_operation->start();
      loop.exec();
      m_operation->wait();
      delete m_operation;
      m_operation = nullptr;

      if (success) { QMessageBox::information(...); this->close(); }
      else         { QMessageBox::warning(...); }
  }
  ```

  `set_progressbar_value(int)` becomes the slot: `ui->progressbar->setValue(value)`, and
  `emit external_logger(value)` when it actually changed — unchanged from today's behavior,
  just invoked via a signal instead of a direct call from the same thread.

  **Public shape is unchanged**: the dialog's signals (`LOG_E/W/I/D`, `external_logger`) and
  `run()`'s blocking contract are identical to today, so `mainwindow.cpp`'s ~30 call sites and
  the `connect_signals_and_run_module()` template need no changes.

### Why `QEventLoop` instead of an async redesign

The `QEventLoop` inside `run()` is the load-bearing idea in this design: it lets `run()` block
its caller (preserving every assumption `mainwindow.cpp` makes) while the GUI thread's *real*
event loop — not a manual `processEvents()` spin — services repaints, `closeEvent`, and the
worker's `Qt::BlockingQueuedConnection` `confirm()` calls. This is the standard Qt idiom for
"block this call until a background thread finishes, while keeping the GUI genuinely
responsive," and it's what makes this a pure internal restructuring of the 30 modules rather
than a `mainwindow.cpp`-wide rewrite.

### Compat shim deletion

With the 30 modules' `serial->` calls now genuinely originating from `XxxOperation`'s worker
thread (not the GUI thread), `serial_port_actions.cpp`'s `waitForDone()` no longer needs the
`qApp`-thread pump branch for its heaviest caller. The shim (the `if (qApp thread) { pump }
else { done->acquire(); } ` branch and the now-unnecessary `#include <QCoreApplication>` pump
logic) is deleted; every caller — including `dtc_operations`/`dataterminal`/`hexcommander` —
goes through the same `done->acquire()` path regardless of which thread it's called from.

## Threading model and data flow

- **GUI thread:** `FlashEcuXxx` — `setupUi`, `show()`/`close()`, all `QMessageBox` calls
  (both the pre-flight one in `run()` and the mid-operation ones relayed through `confirm()`),
  `ui->progressbar->setValue()`, `closeEvent()`.
- **Worker thread (`XxxOperation`, one `QThread` per in-flight operation):** all `serial->`
  calls, all protocol logic, `confirm()`'s blocking-queued round trip back to the GUI thread.
- **Signals:** `LOG_E/W/I/D` and `progressChanged` are ordinary `connect()` calls between
  objects on different threads — Qt's `Qt::AutoConnection` resolves them to
  `Qt::QueuedConnection` automatically based on the emitting thread at emit time (same
  resolution spec 1a relies on for `SerialBackend`'s signals), no explicit connection type
  needed.
- **Stop path:** `closeEvent()` sets the atomic flag via `requestStop()` and returns
  immediately; the worker notices it at the same points `kill_process` is checked today
  (loop-iteration boundaries in `read_mem()`/`write_mem()`), unwinds, and emits
  `operationFinished(false)`, which unblocks `run()`'s `QEventLoop`.
- **Lifecycle:** `XxxOperation` is heap-allocated by `FlashEcuXxx::run()`, started, waited on,
  and deleted within the same `run()` call — no cross-call lifetime concerns, no change to
  `FlashEcuXxx`'s own stack-allocated lifetime as managed by `mainwindow.cpp`.
- **Error handling:** unchanged from today — same `STATUS_SUCCESS`/`STATUS_ERROR` semantics
  internally (mapped to `execute()`'s `bool` return / `operationFinished`'s `bool` argument),
  same final success/failure `QMessageBox`, same `ecuCalDef->FullRomData` population before
  `run()` returns.

## Testing

**Per-`Operation` unit tests** (new), for at least the modules with the most distinctive
protocol logic (Mitsu M32R CAN's security-access seed/key exchange and multi-stage
upload-and-commit is the most complex): drive `execute()` against a fake `SerialBackend`
(spec 1a's `backendFactoryForTests` seam) with scripted responses — success path, wrong
response from ECU, timeout, `requestStop()` mid-loop terminating the operation. `confirm()`'s
injectable prompt callback lets these tests auto-answer without a real modal dialog.

**Thread-safety test** (new): confirm a `FlashOperationWorker` subclass's `serial->` calls
never originate from the GUI thread (reuse the `qInstallMessageHandler`-based affinity-warning
check from spec 1a's facade tests), and that `confirm()`'s `Qt::BlockingQueuedConnection`
round trip correctly blocks the worker and delivers the GUI thread's answer.

**Regression net** (existing, must stay green): `tests/tests.pro`, `serial_crash_tests.pro`,
`serial_backend_tests.pro`.

**Bench gate:** protocol behavior is unchanged by design, but real-hardware re-verification
per migrated module is still required before that module's next live flash — this is a
threading refactor of code with no fake-ECU test infrastructure, and automated tests cannot
substitute for it. Track per-module bench re-verification the same way spec 1a's landing was
tracked in `docs/logging-engine-tech-debt.md`.

## Out of scope

- `dtc_operations`, `dataterminal`, `hexcommander` migration (explicit scope decision above).
- Narrowing/splitting `SerialPortActions`'s public API.
- Any change to protocol behavior, prompt wording, timeout values, or I/O sequencing.
- `FileActions` `QWidget` split (separate, already-deferred spec per `logging-engine-tech-debt.md`).

## Risks

- **Mechanical scale.** 30 modules each need an `Operation` class carved out of existing,
  protocol-sensitive, hardware-validated code. Mitigation: the split is a relocation
  (move-and-rename), not a rewrite — method bodies are unchanged; per-module unit tests catch
  relocation mistakes (wrong member captured, wrong signal emitted) before bench time.
- **`confirm()`'s blocking-queued call deadlocking.** If a future edit ever calls `confirm()`
  from the GUI thread (e.g. a refactor that moves an `Operation` method's call site by
  mistake), `Qt::BlockingQueuedConnection` back to the same thread deadlocks. Mitigation: the
  thread-safety test asserts `Operation` code never runs on the GUI thread; `confirm()` itself
  could assert `QThread::currentThread() != qApp->thread()` in debug builds.
- **Behavioral drift in the mid-operation confirmation flow.** The `QEventLoop` must correctly
  service the nested `Qt::BlockingQueuedConnection` call while itself waiting on
  `operationFinished` — an untested interaction until the first module lands. Mitigation: this
  is an implementation-order question, not a scope split (all 30 still land in this phase) —
  build and bench-test the Mitsu M32R CAN module first (most complex prompt/I/O interleaving,
  already the most-documented protocol in this codebase) as the pattern proof, then apply the
  same transformation to the remaining 29 with higher confidence.
- **Compat shim deletion regressing `dtc_operations`/`dataterminal`/`hexcommander`.** Accepted
  per the scope decision, but worth a manual smoke check (a K-Line session in each) after the
  shim is removed, since the brief UI freeze this introduces was not previously present for
  these dialogs.
