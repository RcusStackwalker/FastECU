# Logging engine — current technical debt and qualification gaps

This document tracks unresolved findings specific to the
`LoggingProtocol`/`LoggingWorker`/`LoggingEngine` architecture. Completed serial
threading and qmake-era build work has been removed; project-wide structural
debt belongs in `docs/tech-debt.md`.

## Real-hardware qualification

The full procedure is in `docs/logging-engine-bench-checklist.md`. The most
consequential open checks are:

1. **Plain-serial logging through `SerialIoThread`.** The backend now owns
   `QSerialPort` on its dedicated I/O thread and headless PTY coverage exists,
   but continuous logging, adapter removal, and clean teardown still need
   confirmation with a real non-J2534 adapter and ECU.
2. **SSM's per-cycle `0xA8 0x01` request.** `SsmLoggingProtocol::poll()` sends
   the request on every cycle. Confirm that supported ECUs treat it as strict
   request/response rather than entering a continuous stream that repeated
   requests could desynchronize.
3. **CDBG logging.** The raw-CAN setup, handshake, security access, and stream
   behavior remain gated by `docs/cdbg-can-logging-bench-checklist.md`.

The worker-thread prompts and progress reporting used by Mitsubishi M32R CAN
flashing are tracked separately in
`docs/colt_czt_47110032_can_bench_checklist.md`.

## Deferred behavior

- **No live GUI indicator for `LoggingStatus::CarNotResponding`.** The worker
  and engine emit the status, but `MainWindow` does not display it beyond the
  warning in the log window.
- **No live reconfiguration.** Changing channels, poll interval, or protocol
  requires a stop/start. Live changes would need an explicit reconfiguration
  path through `LogSessionConfig` and `LoggingWorker`.
- **CDBG startup is not headless.** `CdbgLoggingProtocol::start()` configures
  and opens the real `SerialPortActions` facade before running the driver
  handshake. Moving port/mode setup behind an injected adapter boundary would
  make the complete startup path scriptable.
- **`SessionEndReason::StoppedByUser` is not observed by production callers.**
  `LoggingEngine::stop()` disconnects worker signals before requesting the
  stop, because the caller already knows it initiated the action. The worker
  branch is tested and intentionally remains useful as a standalone contract.

## Minor code-level findings

- `protocol/issm_transport.h` defines `ISsmTransport` in the global namespace,
  unlike `mutdma::IKlineTransport` and `cdbg::ICanTransport`.
- `FastEcuSsmTransport::write()` discards the bytes returned by
  `write_serial_data_echo_check()` and reports the input size unconditionally.
  This matches the K-Line adapter pattern but does not expose an echo failure.
- `MainWindow::handleLoggingSessionEnded()` finds the menu action whose text is
  `Logging`; similar text-based lookup is duplicated in `toggle_realtime()` and
  `toggle_log_to_file()`.
- The MUT/DMA bench helpers `mut_write_memory()` and `mut_read_memory()` still
  live in `log_operations_ssm.cpp`, whose name does not match their ownership.
- `test_ssm_logging_protocol` includes timeout-bounded cases that wait on real
  elapsed time. Keep an eye on its runtime as more timeout scenarios are added.
