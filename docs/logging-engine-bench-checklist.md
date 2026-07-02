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
