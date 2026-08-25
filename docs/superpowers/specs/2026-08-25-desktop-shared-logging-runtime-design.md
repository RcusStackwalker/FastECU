# Desktop Shared Logging Runtime — Design

**Status:** Approved 2026-08-25.

## Context

This design covers delivery sequence step 3 of the approved
[Desktop Quick Application and Configurable CDBG Dashboard design](2026-08-24-desktop-quick-dashboard-design.md):
introduce a generic desktop logging session/protocol API and migrate the
existing Widgets caller through a compatibility adapter.

The current Qt `LoggingEngine` already owns `LoggingWorker` lifecycle and
GUI-thread event delivery, but its start contract is coupled to
`DesktopLoggingSnapshot`, a protocol-ID registry, and factories that receive
legacy parallel-list state. The future QtQuick application needs the same Qt
worker runtime without depending on backend definitions, Widgets models, or
legacy transport selection.

Before the extraction, three current runtime inconsistencies will be fixed as
an independently reviewable prerequisite based on `master`:

- rejected starts will report structured failures consistently;
- explicit user stop will have defined, exactly-once completion semantics; and
- public runtime types will move from the global namespace into
  `fastecu::desktop::logging`.

That cleanup is merged into `markelov/alternative-ui` before the architectural
extraction begins. Keeping it separate makes behavior changes attributable and
lets the extraction preserve an already-tested lifecycle contract.

## Goals

- Make the Qt logging runtime independent of `DesktopLoggingSnapshot`, backend
  definitions, protocol selection strings, and concrete transports.
- Start a run from an owned portable `LoggingSession` and an owned
  `LoggingProtocol`.
- Preserve joined shutdown, GUI-thread delivery, diagnostics, recoverable state
  reporting, and terminal error classification.
- Move all legacy protocol construction and sample application behind a
  Widgets-only compatibility component.
- Keep the Widgets application behaviorally stable except for the separately
  reviewed lifecycle cleanups.
- Leave both desktop application targets buildable after the prerequisite and
  extraction.

## Non-goals

- Configurable CDBG request/reply identifiers, timing, or stream profiles.
- Adapter discovery, adapter resolution, or session-local adapter overrides.
- Moving CDBG serial setup behind a headless connection service.
- A dashboard session builder, QtQuick controller, or QML caller.
- Changes to `LoggingUseCase`, protocol polling behavior, or transport
  implementations.
- Live channel or policy reconfiguration.
- Hardware qualification beyond preserving the existing bench-test boundary.

These concerns belong to delivery sequence step 4 or later.

## Chosen architecture

The reusable boundary remains a Qt desktop-platform component because its
purpose is to adapt the portable `LoggingUseCase` to `QThread` and queued Qt
signals. A second non-Qt runtime core would duplicate responsibilities already
owned by `LoggingUseCase`.

```text
MainWindow
    -> LegacyLoggingCoordinator
         -> legacy snapshot/value adapters
         -> LegacyLoggingProtocolFactory
              -> concrete desktop transports and serial setup
         -> LoggingEngine
              -> LoggingWorker
                   -> portable LoggingUseCase
```

`LoggingEngine` is the shared runtime. It owns no protocol registry and has no
knowledge of legacy models. `LegacyLoggingCoordinator` is the Widgets
compatibility boundary. It owns selection, construction, legacy mapping state,
and translation between runtime events and existing UI operations.

This split is preferred over start overloads on one engine because overloads
would preserve legacy dependencies at the shared boundary. It is preferred
over a new non-Qt runtime layer because the portable use case already provides
that layer.

## Generic runtime contract

### Run input and ownership

The generic start input is a move-only value with this logical shape:

```cpp
struct LoggingRun
{
    fastecu::logging::LoggingSession session;
    std::unique_ptr<fastecu::logging::LoggingProtocol> protocol;
};
```

The final name may follow local naming conventions, but the ownership contract
is fixed. `LoggingEngine::start(LoggingRun)` consumes the complete run
atomically. A successful start transfers the session and protocol to the
engine. They remain alive until cancellation and worker join have completed.
The engine rejects a null protocol and a start attempted while another run is
active.

The engine constructs `LoggingWorker` from the moved session and the owned
protocol. The worker may hold a non-owning protocol pointer because the engine
owns the protocol for strictly longer than the joined worker lifetime. Neither
the worker nor the engine receives `DesktopLoggingSnapshot`.

### Events

The shared engine exposes namespaced Qt events for:

- batches of portable `LogSample` values;
- recoverable `Running` and `CarNotResponding` status;
- one terminal completion with a `SessionEndReason` and detail; and
- diagnostic messages with their original severity.

Worker-to-engine connections use queued GUI-thread delivery. The engine owns
portable-to-desktop terminal classification. It does not apply samples to a UI
model or decide which dialog to show.

### Terminal lifecycle

Every accepted run produces exactly one terminal completion. Cleanup and join
finish before completion observers run, so a completion observer may safely
start another run.

`stop()` is synchronous at the public contract boundary:

1. mark the active run as explicitly stopped;
2. request cancellation;
3. join the worker;
4. release the worker, protocol, and session state; and
5. emit `StoppedByUser` exactly once before returning.

Calling `stop()` without an active run is a no-op. Repeated calls cannot emit a
second completion. Destruction uses the same joined cleanup but does not emit
events to observers during object teardown.

A `Cancelled` result caused by explicit stop maps to `StoppedByUser`. An
unexpected cancellation without a pending explicit stop is classified as
`RuntimeFailed`, preventing unrelated cancellation defects from being hidden
as user intent.

Other terminal errors retain the existing classification:

| Portable outcome | Desktop completion |
|---|---|
| `Disconnected` | `AdapterDisconnected` |
| Other failure before `Running` | `HandshakeFailed` |
| Other failure after `Running` | `RuntimeFailed` |

`CarNotResponding` remains recoverable status. It does not end a run.

## Master-based lifecycle cleanup

The prerequisite change is based on `master` and does not perform the generic
runtime extraction. It makes the current registry-based engine obey the final
lifecycle semantics first.

### Consistent start failures

Every rejected start returns a structured error and reports the failure once.
This includes:

- another run already being active;
- an unregistered protocol ID;
- a protocol factory returning null;
- a protocol factory returning an error; and
- a protocol factory throwing a standard or unknown exception.

The start result must not require callers to infer whether an error was already
reported from a separate `failure_reported` flag. The cleanup replaces that
ambiguous two-axis result with one success-or-error contract and updates the
Widgets caller to restore state in one failure path.

### Explicit stop completion

The current unused `StoppedByUser` value becomes part of the production
contract. The Widgets completion handler performs common state cleanup for it
but shows no warning dialog. Manual cleanup currently duplicated around
`stop()` moves into that common terminal path, while file closure and other
actions that must precede cancellation remain at the caller.

### Namespacing

`LoggingStatus`, `SessionEndReason`, the start input/result types, protocol
factory aliases, `LoggingEngine`, and `LoggingWorker` move under
`fastecu::desktop::logging`. This is a mechanical API cleanup with compile-time
coverage of all callers.

The cleanup receives its own tests and review, then is merged into
`markelov/alternative-ui`. Step 3 starts only after that merge is green.

## Widgets compatibility boundary

### Legacy protocol factory

`LegacyLoggingProtocolFactory` owns the existing protocol selection and
concrete construction rules for MUT/DMA, CDBG, and SSM. It receives the data
needed by those existing factories, including the session channels and
legacy-only SSM response offsets. It may use narrow callbacks or injected
dependencies for current `MainWindow` state and `SerialPortActions`, avoiding a
dependency from the factory back to the full window.

This step preserves current construction ordering. In particular, existing
CDBG serial configuration and port opening remain synchronous factory work.
Making that path configurable and independently headless is deferred to the
connection integration design.

### Legacy logging coordinator

`LegacyLoggingCoordinator` owns one legacy run at a time. On start it:

1. determines the selected portable protocol ID, legacy filter, and existing
   logging policy;
2. creates one `DesktopLoggingSnapshot` from `LogValuesStructure`;
3. retains the snapshot's legacy mapping data;
4. constructs the concrete protocol through `LegacyLoggingProtocolFactory`;
5. moves the snapshot's `LoggingSession` and the protocol into `LoggingRun`;
   and
6. starts `LoggingEngine`.

The snapshot representation may be separated into portable session and legacy
mapping values to make the move explicit. There must be no copied second
session solely to preserve legacy state.

On samples, the coordinator applies each stable-ID sample through the existing
value adapter, then emits narrow notifications for the current value repaint
and logging-to-file operations. It does not own menus or dialogs.

On terminal completion, the coordinator clears its retained snapshot before
forwarding the event to `MainWindow`. Start failure follows the same invariant:
no retained snapshot remains after failure. `MainWindow` owns only visible UI
effects, including warning selection and menu state.

### Dependency boundaries

The shared runtime Bazel target may depend on Qt Core, portable logging use-case
targets, portable ports, and the Qt diagnostic adapter. It must not depend on:

- `src/backend/definitions`;
- `logging_snapshot_adapter` or `logging_value_adapter`;
- `SerialPortActions` or concrete desktop transports;
- `src/ui/desktop`; or
- QtQuick/QML targets.

The Widgets compatibility target owns those legacy dependencies. This makes a
future QtQuick composition root able to construct a portable session and
protocol and call the shared runtime without linking legacy definitions.

## Data flow

```text
legacy logging action
    -> coordinator snapshots current values
    -> legacy factory builds concrete protocol
    -> coordinator moves session + protocol into engine
    -> engine starts worker
    -> worker runs portable LoggingUseCase
    -> queued samples arrive at engine
    -> coordinator applies samples through stable-ID mapping
    -> MainWindow repaints and optionally writes its log file
```

Terminal flow is:

```text
worker terminal result or explicit stop
    -> engine cancels/joins and releases owned run
    -> engine emits exactly one classified completion
    -> coordinator clears legacy snapshot
    -> MainWindow restores menu/state and optionally shows a warning
```

## Error handling and recovery

- Snapshot validation failure prevents protocol construction and leaves no
  active coordinator state.
- Protocol construction failure prevents engine start and uses the common
  structured start-error path.
- A rejected engine start returns ownership cleanup through normal move-only
  destruction and leaves the engine idle or preserves its already-active run.
- Sample-application errors are diagnostic events. They do not mutate an
  unrelated row and do not terminate acquisition solely because a legacy UI
  mapping is missing.
- Adapter disconnect and runtime failure retain their existing warning
  behavior in `MainWindow`.
- Explicit stop performs common cleanup but never shows a warning.
- No error path silently restarts, changes logging policy, or changes protocol
  selection.

## Testing

### Prerequisite cleanup tests

- Characterize lifecycle and signal ordering before behavior changes.
- Verify structured rejection for active-run, unknown-registration, null
  factory, returned error, standard exception, and unknown exception cases.
- Verify one terminal completion for every accepted run.
- Verify synchronous joined stop, exactly one `StoppedByUser`, and no-op
  repeated stop.
- Verify a completion observer can immediately start a new run.
- Verify destruction joins without publishing teardown events.
- Verify the Widgets caller restores menu and logging state for every failed
  start and shows no dialog for user stop.
- Compile all callers against namespaced public types.

### Shared runtime tests

`LoggingEngine` and `LoggingWorker` tests use `ScriptedLoggingProtocol` and do
not link legacy logging adapters or backend definitions. They cover:

- move-only session/protocol ownership;
- null protocol and active-run rejection;
- startup, queued sample delivery, and state delivery;
- diagnostic severity forwarding;
- pre-running and post-running error classification;
- adapter disconnect;
- explicit and unexpected cancellation;
- joined teardown and exactly-once completion; and
- restart after completion.

### Widgets compatibility tests

- MUT/DMA, CDBG, and SSM protocol selection and policy preservation.
- SSM response-offset preservation.
- MUT/DMA enabled-row filtering and CDBG channel selection.
- Stable-ID application after legacy row reordering.
- Snapshot lifetime across samples and cleanup before terminal forwarding.
- Factory errors, null protocols, thrown exceptions, and engine rejection.
- Repaint and logging-to-file notification after successful sample batches.
- No retained snapshot after start failure, stop, disconnect, or runtime
  failure.

### Build and regression gates

- Build and test `//:fastecu`.
- Build and test `//:fastecu-desktop-quick`, although no QtQuick caller is
  introduced in this step.
- Run the focused logging adapter, worker, engine, and coordinator tests.
- Run portable closure, formatting, clang-tidy, and repository-standard test
  gates affected by the changed dependency graph.
- Confirm through Bazel queries that the shared runtime no longer reaches
  backend definitions or Widgets integration.

## Delivery sequence

1. Create the lifecycle cleanup from `master`, add characterization and new
   contract tests, and obtain independent review.
2. Merge the cleanup into `markelov/alternative-ui` and run both desktop build
   gates.
3. Change `LoggingEngine` to the owned `LoggingRun` API and remove its registry
   and snapshot dependencies.
4. Add `LegacyLoggingProtocolFactory` and migrate the three existing concrete
   protocol builders.
5. Add `LegacyLoggingCoordinator`, move snapshot retention and sample
   application out of `MainWindow`, and reduce the window to visible UI
   effects.
6. Split Bazel targets at the generic/legacy boundary and verify the dependency
   graph.
7. Run focused, desktop-target, static-analysis, and repository regression
   gates.

Every item leaves the Widgets application buildable. No QtQuick caller or
delivery step-4 connection behavior is included.

## Acceptance criteria

- The shared `LoggingEngine` starts only from an owned portable session and an
  owned protocol.
- The shared runtime has no dependency on backend definitions, legacy logging
  adapters, concrete transports, `SerialPortActions`, or Widgets UI code.
- The Widgets application starts MUT/DMA, CDBG, and SSM logging through the
  compatibility coordinator with current selection, channel, offset, and value
  update behavior preserved.
- Every rejected start has one structured failure path.
- Every accepted run has exactly one terminal completion.
- Explicit stop joins synchronously, emits `StoppedByUser` once, clears state,
  and shows no warning.
- Runtime cleanup completes before terminal observers run, and immediate
  restart is safe.
- Public desktop logging runtime types reside in
  `fastecu::desktop::logging`.
- `//:fastecu` and `//:fastecu-desktop-quick` build after the migration.
- Configurable CDBG and adapter-resolution work remains deferred to delivery
  step 4.
