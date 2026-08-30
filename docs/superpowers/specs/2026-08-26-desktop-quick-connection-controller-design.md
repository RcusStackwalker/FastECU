# Desktop Quick Connection Service and Controller — Design

**Status:** Approved 2026-08-26.

## Context

The approved [Desktop Quick Application and Configurable CDBG Dashboard
design](2026-08-24-desktop-quick-dashboard-design.md) divides delivery into
focused checkpoints. The QtQuick shell, portable `.ohd` foundation, shared Qt
logging runtime, and configurable CDBG session foundation establish the build,
document, worker-lifecycle, and portable session contracts.

The next checkpoint connects those pieces to local hardware and exposes the
first functional connection workflow to QtQuick. It owns local adapter
discovery and resolution, document-driven raw-CAN setup, transport and protocol
construction, and explicit Connect/Disconnect presentation state. It does not
yet render dashboard cards or consume live samples.

## Goals

- Discover compatible local J2534 and SocketCAN adapters through normalized,
  presentation-safe descriptors.
- Resolve an `.ohd` preferred adapter only when it identifies exactly one
  available local adapter.
- Let a user explicitly select a session-local adapter when preference
  resolution is missing or ambiguous.
- Configure raw CAN from the validated document profile and construct one
  complete logging run with deterministic ownership.
- Expose an explicit Connect/Disconnect workflow and stable connection state to
  QtQuick through a narrow controller.
- Preserve the existing Widgets application's fixed Colt setup and behavior.
- Release all hardware and worker resources on every failure and disconnect
  path.

## Non-goals

- Remote, WebSocket, or Qt Remote Objects adapter connections.
- Generic serial devices that cannot provide raw CAN.
- Automatic connection when a document is opened or restored at startup.
- Persisting a session-selected adapter or mutating preferred-adapter hints.
- Loading, saving, importing, or editing `.ohd` documents.
- Dashboard card models, live values, sparklines, gauges, or sample history.
- SSM, MUT/DMA, ISO 15765, diagnostics, flashing, or other writable workflows.
- Reworking the existing Widgets adapter-selection UI.

## Chosen architecture

The checkpoint separates hardware preparation from presentation and worker
lifecycle:

```text
QML connection controls
    -> DashboardConnectionController
         |-- DesktopConnectionService
         |    |-- LocalAdapterDiscovery
         |    |-- local adapter factory
         |    |-- FastEcuCanTransport
         |    `-- CdbgLoggingProtocol
         `-- LoggingEngine
```

`DesktopConnectionService` owns discovery, resolution, configuration, opening,
and construction. A successful call returns a complete
`fastecu::desktop::logging::LoggingRun` containing the portable session and an
owned CDBG protocol whose transport owns the opened adapter for the run's full
lifetime. The service never starts a worker and retains no active connection.

`DashboardConnectionController` owns one `LoggingEngine`, coordinates explicit
user actions, and maps service and engine outcomes into presentation state. It
contains no `SerialPortActions`, CAN configuration calls, protocol objects, or
transport types. QML receives only this controller and presentation-safe
candidate data.

This boundary is preferred over making the connection service own the logging
engine because adapter preparation and run lifecycle remain independently
testable and reusable. It is preferred over controller-level orchestration of
individual factories because QML presentation code remains independent of
platform concepts.

## Local adapter discovery

### Descriptor model

`LocalAdapterDiscovery` returns normalized `LocalAdapterDescriptor` values for
compatible local adapters. Each descriptor contains:

- the portable kind: J2534 or SocketCAN;
- normalized vendor and display name used for preference matching;
- a user-facing label; and
- an opaque process-local candidate ID used to select and open that discovery
  result.

The candidate ID may encode or reference machine-local information, but it is
never exposed as portable document data and is never serialized. Discovery
filters out ordinary serial ports and every remote adapter mode.

J2534 and SocketCAN enumeration remain platform-specific behind narrow
interfaces. A platform may return no candidates for an unsupported kind
without failing discovery for supported kinds. Failure to enumerate one
provider is retained in diagnostics; candidates from another successfully
enumerated provider remain usable.

### Discovery generations

Every discovery result has a generation. An explicit selection is valid only
for the generation that produced it. If the adapter list is refreshed or a
candidate disappears, the old candidate ID is rejected and discovery runs
again. This prevents an opaque identifier from selecting a different device
after local hardware changes.

## Adapter resolution

Resolution is deterministic:

1. If the user supplied a candidate ID from the current generation, resolve
   that exact local candidate.
2. Otherwise, if the document has a preferred adapter, compare kind,
   normalized vendor, and normalized display name.
3. Auto-select only when all preference fields identify exactly one candidate.
4. Return selection-required for no match, multiple matches, or an absent
   preference.
5. Never silently select the first discovered adapter.

The normalized comparison is case-insensitive for human-readable vendor and
display-name text and ignores surrounding whitespace. It does not use fuzzy or
substring matching. Adapter kind must match exactly.

The user's explicit choice is a session override. The connection service and
controller do not copy it into `DashboardDocument`. A later document workflow
may offer an explicit action that stores the selected descriptor's portable
hints and marks the document dirty.

## Connection preparation service

### Outcome contract

The service returns one typed outcome:

```text
Prepared(LoggingRun, selected descriptor)
SelectionRequired(candidate descriptors, discovery generation, reason)
Failed(Error)
```

Selection-required is not represented as a generic configuration or internal
error. It is an expected interaction state and may contain an empty candidate
list when no compatible local adapter exists.

The logical operation is:

```text
prepare_run(document, optional selected candidate)
    -> prepare_dashboard_session(document)
    -> discover compatible local adapters
    -> resolve preferred or selected adapter
    -> configure and open adapter
    -> construct FastEcuCanTransport
    -> construct CdbgLoggingProtocol
    -> return LoggingRun
```

Dashboard session preparation occurs before hardware access, so document,
channel, conversion, frame-capacity, and protocol-profile errors cannot open an
adapter. A document with no cards is not connectable because it produces no
useful logging run.

### Configuration

The selected adapter is configured from the validated document profile:

- disable ISO 14230 mode and its header handling;
- enable raw CAN;
- disable ISO 15765 mode;
- select 11-bit or 29-bit CAN identifiers;
- apply the document bitrate; and
- configure reply filtering for the document reply identifier.

The existing fixed `cdbg_serial_setup` path becomes a profile-driven validated
setup API rather than being duplicated. The Widgets compatibility caller
continues to supply the Colt defaults, preserving its effective setup and wire
traffic.

Configuration steps that an adapter supports before opening run first. J2534
operations that require an open channel run immediately after opening. A
provider reports `Unsupported` when it cannot represent a validated bitrate,
identifier width, or reply filter; it must not substitute, round, or silently
fall back.

### Ownership and cleanup

Preparation is atomic from the caller's perspective. It returns either a fully
owned run ready for `LoggingEngine::start()` or no open adapter.

The concrete adapter handle is owned through the transport/protocol chain for
the complete run. Scope-bound cleanup closes or releases it if configuration,
opening, transport construction, protocol construction, or result assembly
fails. If `LoggingEngine::start()` rejects a prepared run, destruction of the
unconsumed run closes the adapter. The service retains no borrowed pointer into
discovery data and no reusable half-configured adapter.

## QtQuick connection controller

### Public state

The controller exposes these stable presentation states:

```text
Disconnected
Connecting
AdapterSelectionRequired
Running
CarNotResponding
Disconnecting
Failed
```

It provides read-only QML properties for:

- current state;
- concise status text;
- expandable technical detail;
- selected-adapter label;
- candidate-adapter model;
- discovery generation;
- `canConnect`;
- `canDisconnect`; and
- `needsAdapterSelection`.

QML does not derive state-machine rules itself. The candidate model exposes an
opaque candidate ID and presentation label, not platform handles or adapter
objects.

The controller exposes these invokable actions:

- `connectDashboard()`;
- `connectWithAdapter(candidateId)`;
- `refreshAdapters()`; and
- `disconnectDashboard()`.

The controller receives the current validated `DashboardDocument` through its
composition boundary. It does not own document loading, saving, or editing.

### Connect flow

Hardware connection always requires an explicit Connect action. Opening or
restoring a document never invokes the service automatically.

`connectDashboard()` is rejected when no document is available, the document
has no cards, or a connect or active run already exists. Otherwise the
controller enters `Connecting` and asks the service to prepare a run.

Selection-required changes the state to `AdapterSelectionRequired` and exposes
the returned candidates. An empty candidate list shows a no-compatible-adapter
state with Refresh. Selecting a candidate calls `connectWithAdapter()` with the
current discovery generation. A stale or vanished selection triggers fresh
discovery and returns to selection-required rather than opening another
device.

When preparation succeeds, the controller records the selected-adapter label
and passes the run to `LoggingEngine::start()`. It does not enter `Running`
merely because the adapter opened. `Running` begins only when the engine emits
its running status after the CDBG handshake succeeds.

### Runtime and disconnect flow

`CarNotResponding` is a recoverable connected state. A later valid sample moves
the controller back to `Running`. This checkpoint listens to engine status and
terminal events but does not forward `valuesUpdated` into card models.

Terminal engine completion maps to a stable presentation outcome and releases
the active run. Handshake failure, adapter disconnection, and runtime failure
enter `Failed` with reconnect available. An explicit stop settles in
`Disconnected` without a warning.

Disconnect uses the engine's synchronous, joined `stop()` contract. The
controller enters `Disconnecting`, calls `stop()`, and handles its exactly-once
`StoppedByUser` completion. Calling disconnect without an active run is
harmless. Destruction performs joined teardown while suppressing late
presentation events.

## Initial QML surface

The existing QtQuick shell gains a functional connection panel or bottom-bar
control containing:

- Connect or Disconnect according to controller state;
- current connection status;
- selected adapter label;
- candidate selection when required;
- adapter refresh; and
- concise error text with optional technical detail.

Unavailable actions are disabled. The UI does not contain transport settings,
platform handles, or writable ECU operations. It does not add placeholder
dashboard cards or consume live values.

## Errors and recovery

The platform layer preserves portable error kinds and includes a stable
operation name in details. The controller owns user-facing summaries.

| Condition | Presentation behavior |
|---|---|
| No compatible local adapter | Selection-required with empty state and Refresh. |
| Missing or ambiguous preference | Selection-required with candidate list. |
| Candidate vanished or generation is stale | Refresh discovery and require a new selection. |
| Unsupported adapter configuration | Failed; identify the unsupported profile setting. |
| Adapter open failure | Failed; retain candidates and permit retry or another choice. |
| CDBG handshake failure | Failed; keep the selected label and offer reconnect. |
| Runtime adapter disconnection | Failed; release the run and offer rediscovery. |
| ECU silence | Remain connected in `CarNotResponding`; allow configured retries. |
| Valid sample after silence | Return to `Running`. |
| Explicit disconnect | Joined cleanup followed by `Disconnected`; no warning. |
| Internal failure | Stop safely and show a concise summary with technical detail. |

No error changes the document, preferred-adapter hints, or local settings.
Retry is always an explicit user action after a terminal failure.

## Testing

### Desktop platform

- Normalize and filter J2534 and SocketCAN discovery results.
- Exclude generic serial and remote adapters.
- Cover provider-specific enumeration failure while retaining candidates from
  another provider.
- Cover unique preference match, no preference, no match, and ambiguous match.
- Cover explicit current-generation selection and stale candidate rejection.
- Prove comparison normalization without fuzzy or substring matching.
- Verify exact raw-CAN mode, bitrate, identifier width, and reply-filter setup.
- Cover unsupported settings without substitution or hardware leakage.
- Inject failure at every configuration, open, transport, and protocol stage
  and prove resources are released.
- Produce a complete fake-backed `LoggingRun` and exercise it through the
  shared logging engine.
- Preserve the existing fixed Colt Widgets setup through regression tests.

### Controller and QML

- Prove document restoration never connects automatically.
- Cover every controller state transition and derived action property.
- Cover selection-required with populated and empty candidate models.
- Cover selection, refresh, and stale-generation behavior.
- Cover preparation and engine-start failures.
- Cover handshake failure, runtime disconnection, ECU silence, and recovery.
- Prove explicit disconnect is joined and terminal completion occurs once.
- Prove repeated disconnect and destruction are safe.
- Reject connect without a document or without cards.
- Load the connection QML offscreen with fake services and exercise Connect,
  selection, Refresh, and Disconnect bindings.

### Regression gates

- Build and test both `//:fastecu` and `//:fastecu-desktop-quick`.
- Run focused CDBG protocol, dashboard session builder, logging engine, legacy
  logging factory, connection service, controller, and QML smoke tests.
- Run portable-closure and repository formatting checks for affected targets.

## Delivery sequence

1. Add normalized local adapter discovery interfaces and fake providers.
2. Generalize fixed CDBG serial setup into validated profile-driven raw-CAN
   configuration while preserving the Widgets compatibility call.
3. Add local adapter factories and scope-bound opened-adapter ownership.
4. Add `DesktopConnectionService` resolution and complete-run preparation.
5. Add `DashboardConnectionController` and its state-machine tests.
6. Add the connection QML surface and offscreen smoke coverage.
7. Run both desktop builds and focused regression gates.

Each step leaves the Widgets application buildable. Platform components are
tested without QML, and controller tests use service and engine seams without
real hardware.

## Acceptance criteria

- The QtQuick application never opens hardware merely because it restored or
  received a document.
- Explicit Connect discovers only local J2534 and SocketCAN adapters.
- A preferred adapter is selected automatically only when it matches exactly
  one current local candidate.
- Missing, ambiguous, stale, or unavailable preferences require explicit user
  selection and never fall back to the first device.
- A selected adapter is configured exactly from the validated `.ohd` raw-CAN
  profile and produces a complete owned logging run.
- QtQuick can connect, show running and recoverable-silence state, disconnect
  with joined cleanup, and display actionable failure state.
- Session-selected adapters are not persisted or written into the document.
- Every failed preparation and terminal run releases hardware ownership.
- No QtQuick component receives transport, serial, protocol, or writable ECU
  APIs.
- Both desktop applications remain buildable and the Widgets CDBG setup remains
  behaviorally unchanged.

## Implementation-planning boundary

The implementation plan for this design covers local adapter discovery,
profile-driven raw-CAN setup, run preparation, the connection controller, and
the minimal QtQuick connection surface. Dashboard card/value presentation and
document workflows remain separate later checkpoints.

## References

- [Desktop Quick dashboard design](2026-08-24-desktop-quick-dashboard-design.md)
- [Portable `.ohd` foundation](2026-08-24-desktop-quick-ohd-foundation-design.md)
- [Desktop shared logging runtime](2026-08-25-desktop-shared-logging-runtime-design.md)
- [Configurable CDBG session foundation](2026-08-26-desktop-quick-cdbg-session-foundation-design.md)
