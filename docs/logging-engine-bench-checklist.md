# Logging engine refactor -- bench verification checklist

Verifies the LoggingEngine/LoggingWorker/LoggingProtocol refactor behaves
correctly against real hardware. Focused Bazel targets under `//tests:` cover
the worker, engine, and protocol logic against scripted transports; this
checklist covers what can only be observed with a real adapter and ECU. Run all
maintained test targets with `bazel test --config=release //tests/...` before
bench testing.

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
   socket notifiers or timers being used from the wrong thread. The serial
   backend now constructs `QSerialPort` on the dedicated `SerialIoThread`; this
   step is the real-hardware verification of that architecture. The headless
   analog is `tests/test_pty_e2e.cpp`.

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
