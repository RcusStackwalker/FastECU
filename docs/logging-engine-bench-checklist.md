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
6. On a plain-serial (non-J2534/non-OpenPort-2.0) adapter specifically: start
   logging and confirm gauges update continuously with no Qt warnings about
   socket notifiers or timers being used from the wrong thread. `SerialPortActionsDirect`
   creates its `QSerialPort` with GUI-thread affinity, and its blocking reads
   pump `QCoreApplication::processEvents()`; now that logging runs on
   `LoggingWorker`'s own thread, cross-thread `QSerialPort` I/O may not deliver
   data correctly. This is separate from the OpenPort 2.0/J2534 path, which
   uses thread-agnostic dylib calls and is expected to be unaffected.

## Regression checks

7. Start a MUT/DMA or Cdbg session (previously never called
   `update_logbox_values()` -- see Task 9's completion notes) and confirm the
   lower-panel gauges actually refresh, not just `logValues->log_value` in
   memory.
8. Confirm no other serial operation (ROM read/flash) can be started while a
   logging session is active, matching the pre-refactor `logging_state` guard.
9. On SSM specifically: let a session run for a while and watch for garbled or
   misaligned frames. `SsmLoggingProtocol::poll()` now re-sends the
   `0xA8 0x01 <addresses>` "continue" request every single poll cycle
   (previously, due to a dead timer, this request was effectively sent only
   once). If the target ECU's `0xA8 0x01` command puts it into a continuous-
   stream response mode rather than a strict request/response mode, re-issuing
   the request mid-stream could desync the response framing. Verify on real
   SSM hardware that repeated `0xA8 0x01` requests behave as clean polling
   (one response per request) rather than causing this.
