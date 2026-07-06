# Logging engine refactor — technical debt and improvement opportunities

Known gaps, deferred risks, and follow-up ideas surfaced while building and reviewing the
`LoggingProtocol`/`LoggingWorker`/`LoggingEngine` architecture. Nothing here blocks the
current code — all are deliberate scope cuts or low-risk findings accepted during review.

## Needs real-hardware verification (already in the bench checklist)

These are tracked in `docs/logging-engine-bench-checklist.md`; listed here too since they're
the most consequential open items:

1. **Direct-serial (non-J2534) backend under the new threading model.** `SerialPortActionsDirect`
   creates its `QSerialPort` with GUI-thread affinity and pumps `QCoreApplication::processEvents()`
   on the calling thread during blocking reads. Now that `poll()` runs on `LoggingWorker`'s own
   thread, cross-thread `QSerialPort` I/O may not deliver data correctly, or may emit "socket
   notifiers used from another thread" warnings. The primary OpenPort 2.0/J2534 path uses
   thread-agnostic dylib calls and should be unaffected — this only risks plain-serial adapters.
2. **SSM's per-cycle `0xA8 0x01` re-send.** `SsmLoggingProtocol::poll()` now re-sends the "continue"
   request every cycle (fixing a dead-timer bug in the original code — see below). If a given SSM
   ECU's `0xA8 0x01` puts it into a continuous-stream response mode rather than strict
   request/response, re-issuing it mid-stream could desync response framing. Needs confirmation on
   real hardware before first live SSM flash.

## Flash operation worker migration (phase 1b)

`FlashEcuMitsuM32rCan` is the pattern-proof for the flash-module worker-thread
migration.
The real-hardware re-verification this migration needs before its next live flash
is tracked as part of the module's existing bench-qualification gate — see Step 4
("Erase trigger") in `docs/colt_czt_47110032_can_bench_checklist.md` for the
QEventLoop/confirm()-under-worker-thread and live-progress-bar checks.

## Deliberately deferred (scope cuts, not oversights)

- **No live GUI indicator for `LoggingStatus::CarNotResponding`.** `LoggingWorker`/`LoggingEngine`
  compute and emit this transition, but `MainWindow` doesn't wire `statusChanged` to anything —
  the only visible signal today is the `LOG_W("Car not responding", ...)` line in the log window.
  Adding a status indicator widget was explicitly scoped out during design (no placeholder UI).
- **No live reconfiguration.** Changing channels, poll interval, or protocol requires a full
  stop/start of the session (per the plan's explicit scope decision). If per-channel add/remove
  without dropping a session is ever wanted, `LogSessionConfig`/`LoggingWorker` would need a new
  "reconfigure" command path.
- **`CdbgLoggingProtocol::start()`'s CAN-mode-setup + handshake path has zero unit-test coverage.**
  It calls the real `SerialPortActions::open_serial_port()`, an OS-level operation inappropriate
  for a headless unit test — deliberately deferred to bench verification (checklist item 3 for
  Cdbg). `SsmLoggingProtocol`/`MutDmaLoggingProtocol`'s `start()` paths don't have this gap since
  neither touches `SerialPortActions` directly.
- **`SessionEndReason::StoppedByUser` is effectively unreachable in practice.** `LoggingWorker`
  always emits it on a clean exit, but `LoggingEngine::stop()` disconnects the worker's signals
  before requesting the stop specifically so this doesn't re-propagate to `MainWindow` (the caller
  already knows it asked for this). The enum value and code path exist and are exercised by unit
  tests, but no production code ever observes this particular emission. This is intentional, not
  a bug, but a future reader may wonder why the branch is "dead" downstream.

## Minor code-level findings (accepted during review, not fixed)

- `logging/protocols/issm_transport.h`'s `ISsmTransport` has no namespace, unlike its siblings
  `mutdma::IKlineTransport`/`cdbg::ICanTransport`. Cosmetic inconsistency.
- `FastEcuSsmTransport::write()` discards the bytes returned by `write_serial_data_echo_check()`
  and unconditionally returns `data.size()` — matches the pre-existing `FastEcuKlineTransport`
  pattern, but neither actually verifies the echo. Worth revisiting if echo-verification semantics
  are ever needed for adapter-loss detection.
- `SsmLoggingProtocol::poll()`'s per-channel copy loop bounds against `(uint8_t)received.length()`
  — truncates for a hypothetical response ≥256 bytes. Transcribed verbatim from the original SSM
  code; unreachable given real SSM payload sizes, but a latent landmine if that ever changes.
- `MainWindow::handleLoggingSessionEnded()`'s "find the QMenu action whose text is 'Logging' and
  uncheck it" loop duplicates the same pattern already inlined in `toggle_realtime()` and
  `toggle_log_to_file()`. A small `findMenuActionByText(QString)` helper would deduplicate all
  three call sites — pre-existing style in this codebase, not new debt from this refactor, but now
  present a third time.
- `mut_write_memory()`/`mut_read_memory()` (bench utilities for direct RAM read/write over MUT/DMA)
  now live in `log_operations_ssm.cpp`, purely because that's where the code that used to hold them
  (`log_operations_mitsubishi.cpp`) got deleted from. The filename no longer reflects their
  contents. A future cleanup could relocate them to a dedicated `mut_dma_bench_utilities.cpp` or
  similar.

## Structural / build-system debt

- **`tests/tests.pro` grew from a lightweight `QT += core testlib` binary into one that links
  QtWidgets, QtXml, QtRemoteObjects, QtWebSockets, and the full `SerialPortActions` facade** —
  because `FileActions` (needed by `convertRomRaiderValue()`/`SsmLoggingProtocol`, per the plan's
  own interface design) turns out to be a `QWidget` subclass, and `CdbgLoggingProtocol`'s
  constructor needs a real `SerialPortActions*`. This was the correct minimal fix given the
  existing interfaces, but it means what used to be a fast, dependency-free unit-test binary now
  carries a much heavier link surface. If test build/link time becomes a problem, the underlying
  fix is architectural: `FileActions` mixes pure logic (`calculate_value_from_expression`,
  `parse_stringlist_from_expression_string`) with GUI/`QWidget` concerns. Splitting the
  expression-evaluation logic into a plain, non-`QWidget` class would let `convertRomRaiderValue()`
  and its tests drop the `QApplication`/widgets/checksum-module dependency chain entirely.
- **`SerialPortActions`'s blocking-I/O implementation was explicitly not modified** by this refactor
  (`SerialPortActions::read_serial_data()`/`write_serial_data()` still work the same way — bounded
  synchronous calls). The "fake async" `QCoreApplication::processEvents()` pump inside those calls
  was previously a mostly-harmless quirk because it only ever ran on the GUI thread; it's now also
  exercised from `LoggingWorker`'s thread for the direct-serial backend, which is exactly the
  behavior the bench checklist's item 1 needs to validate. **Update:** the thread-safe I/O path has
  since landed: backends now live on a dedicated `SerialIoThread` behind a marshaling facade;
  the GUI-thread compat shim was removed once the flash-op worker migration landed —
  `waitForDone()` now blocks unconditionally regardless of caller thread.
- **`TestSsmLoggingProtocol`'s suite runtime (~1050ms)** comes from its timeout-bounded tests
  genuinely waiting out real wall-clock time (`QElapsedTimer`-bounded reassembly loops with no
  artificial delay to short-circuit in tests). Harmless today; worth watching if more
  timeout-bound protocols are added and the unit suite's total runtime starts to matter.

## Broader project context (not introduced by this refactor, but adjacent)

- The project's own `CLAUDE.md` already tracks a Rust orchestration layer to replace the Make-based
  build in `mmc-patches`, and generalizing `codeinjector` for SH-2/FastECU — unrelated to this
  logging work but worth remembering these exist as separate, already-tracked deferred items.
