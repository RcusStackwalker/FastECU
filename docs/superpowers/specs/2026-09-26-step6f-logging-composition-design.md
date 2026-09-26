# Step 6f: Desktop logging composition

Status: implemented and locally tested; final review and cross-platform qualification pending.

Baseline: `8d55e3ab`, following completion of modularization step 6e.

## Intent and success criteria

Continue the thin desktop shell work in `docs/modularization-plan.md` by
moving logging-protocol construction and registration out of `MainWindow`.
The user selected this increment and approved its ownership boundary and
validation scope. Preserve desktop logging behavior, protocol wire sequences,
error reporting, and support for Windows, macOS, and Linux.

The composition root must supply a logging engine with MUT/DMA, CDBG, and SSM
registered before constructing the window. Factories must operate without a
window. The UI supplies user choices as snapshot data and handles signals.
No new consumer may join the frozen `serial_qt_compat` allowlist.

## Current behavior

`DesktopComposition` owns the serial facade, logging clock, and logging engine.
It destroys the engine before the facade and retains the clock throughout
engine destruction. `MainWindow::setupLoggingEngine()` connects sample and
completion signals, then registers three factories capturing `this`:

- MUT/DMA constructs `FastEcuKlineTransport`, `AlreadyInMode(125000)`, and
  `MutDmaLoggingProtocol` using snapshot channels.
- CDBG configures seven serial settings in order, opens and checks the port,
  then constructs `FastEcuCanTransport` and `CdbgLoggingProtocol`.
- SSM constructs `FastEcuSsmTransport`, reads `ecu_radio_button`, queries the
  OpenPort adapter flag, and constructs `SsmLoggingProtocol` with the clock,
  channels, and response offsets.

`menu_actions.cpp` connects to the ECU if necessary, selects the protocol and
logging policy, builds the snapshot, retains a UI copy, and calls
`LoggingEngine::start()`. The engine invokes the factory synchronously before
starting its worker. Registration itself does not perform hardware I/O.

## Architecture and ownership

Add a dedicated `:logging_protocol_registration` target in
`src/platform/desktop/common/transport`, with
`desktop_logging_protocol_registration.h/.cpp`. Its entry point is:

```cpp
void register_desktop_logging_protocols(
    LoggingEngine& engine, SerialPortActions& serial, fastecu::IClock& clock);
```

Place the function in `fastecu::desktop::logging`; forward-declare the
argument types in the public header. It registers the three existing IDs
through `LoggingEngine::registerProtocol`. Factory closures capture service
references, never UI objects or temporary references. The caller must keep
the serial service and clock alive until the engine and its factories are
destroyed. `DesktopComposition` already satisfies this requirement.

The transport package already owns the concrete adapters and is permitted to
use `serial_qt_compat`. Keeping the helper here avoids adding a forbidden
dependency from the logging package or composition root to that facade.
Give the new target explicit visibility for `apps/desktop` and its own
package's tests; the UI does not consume the production registration helper.
Keep it separate from the base transport target so existing transport
consumers do not acquire logging-runtime dependencies.

`DesktopComposition` calls the helper after creating the engine and connecting
its diagnostic signals. `MainWindow::setupLoggingEngine()` retains its engine
reference and signal connections only. Remove `logging_clock` from
`MainWindowServices` and its fixtures; keep the clock owned by the composition.
Remove factory-only includes and dependencies from UI targets where no other
UI source uses them. Do not claim removal of the whole UI-to-transport edge
unless the remaining consumers actually permit it.

## Session data and flow

Add `bool target_is_ecu = true` to `DesktopLoggingSnapshot`. The default keeps
existing synthetic snapshots usable; the production UI explicitly assigns
`ecu_radio_button->isChecked()` after successful snapshot creation and before
both the UI copy and the call to `start()`. This is a per-run value, not a
reference to live widget state. Snapshot parsing and channel selection stay
in the existing adapter.

The SSM factory uses `snapshot.target_is_ecu` and queries
`serial.get_use_openport2_adapter()` when invoked. The adapter flag remains a
platform observation rather than UI-supplied data. Channels and response
offsets continue to come from the same snapshot. MUT/DMA and CDBG ignore the
target field.

Factories still execute synchronously on the start caller's thread. Backend
protocol work still executes on the existing logging worker, and serial
operations retain the facade's existing thread marshalling. This increment
does not change execution threads or move port opening to application startup.

## Errors and lifecycle

Preserve the current CDBG configuration sequence and argument values:

1. Disable ISO 14230 mode.
2. Disable ISO 14230 headers.
3. Enable raw CAN.
4. Disable ISO 15765.
5. Select 11-bit CAN IDs.
6. Select 500000 baud.
7. Select `MitsuColtCanCdbg::kReplyCanId` as the destination.

Reuse `configure_cdbg_serial` and its existing `InvalidConfig` errors. A
failed step prevents all subsequent setup and open calls. After configuration,
preserve the existing open result and open-state checks, including their
short-circuit order. Failure returns `Disconnected` with
`unable to open CAN adapter for CDBG logging`. Do not introduce rollback or
change port ownership as part of extraction.

Keep factory error/exception/null-result handling in `LoggingEngine::start()`.
The UI continues to restore logging state and display its existing start
failure message. Worker completion, cancellation, stop/restart, queued-event
filtering, and teardown remain governed by the current engine.

## Validation

Use behavioral tests at the extracted boundary, with the existing fake-backed
serial fixture. Run factory and registration tests without `MainWindow` or
real hardware. Avoid adding public factory-map inspection solely for tests.

- Registration performs no serial I/O; all three IDs can be started through
  the public engine API with appropriately scripted backends.
- CDBG emits the existing configuration values in order. Inject failure at
  each setter and verify later operations are absent. Cover empty open result,
  failed open-state check, and successful construction/startup.
- SSM uses the captured ECU/TCU choice and the current adapter flag. Exercise
  both target values and adapter-dependent behavior through observable
  protocol/transport interactions. Preserve channels and response offsets.
- MUT/DMA retains the 125000 already-in-mode initialization and snapshot
  channel selection.
- A UI test supplies its own capturing factory, starts logging through the
  existing UI path, and verifies the chosen target reaches the snapshot.
  `MainWindow` construction must not overwrite injected factories.
- Composition coverage verifies registration is available without creating a
  window, with no actual adapter access required during construction. Retain
  existing immediate teardown and repeated-construction coverage.

Reuse the existing logging-engine tests for factory errors, exceptions, null
returns, active-run rejection, cancellation, stop/restart, and destruction.
The existing generic CDBG setup test remains useful; new tests prove the
extracted production wiring supplies the correct actions and values.

Run the affected transport registration, logging adapter/runtime/worker,
composition, and MainWindow tests. Run `//:serial_compat_allowlist` and
`//:portable_closure`, and build `//:fastecu`. Desktop CI must cover the
supported OS matrix and existing Windows/macOS packaging checks. Implementation
planning will name the new test label alongside existing package-owned labels.

Record a bench checklist covering MUT/DMA start/stop, CDBG setup/start failure
and successful logging, SSM ECU/TCU selection, applicable adapter variants,
and restart/teardown. Automated tests establish regression evidence; hardware
qualification remains separately recorded and must not be inferred from them.

## Scope and alternatives

A broader logging-session controller would also move connection orchestration,
policy selection, and state transitions, but those are separate responsibilities
and substantially expand this increment. Placing factory bodies directly in
`apps/desktop` would expose the full serial facade there and conflict with the
frozen allowlist. The composition-owned platform helper is the selected approach.

This step does not redesign logging protocols, add a generic protocol registry,
replace legacy parallel-list models, change log-file handling, extract BIU,
remove the serial facade, or begin Android integration. Existing ECU connection
logic and protocol/policy selection remain in the UI for this increment.

## Delivery and completion

The implementation plan should separate snapshot plumbing, factory extraction
and composition wiring, and close-out validation into reviewable changes while
keeping each change buildable. Update the modularization plan, design notes,
and relevant tech-debt entry to record the resulting boundary. Distill lasting
decisions into the design notes at close-out, following the existing step 6
documentation convention.

Completion requires all three registrations outside `MainWindow`, explicit
per-run target data, preserved factory behavior and service lifetimes, no
allowlist growth, and passing automated gates. Report hardware checks according
to their actual status. Written-spec approval precedes implementation planning;
this document authorizes no product-code changes by itself.
