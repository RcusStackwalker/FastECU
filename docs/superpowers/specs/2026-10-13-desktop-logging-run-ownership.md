# Desktop logging run ownership

Status: agreed design; proposed implementation. This specification does not
authorize implementation or behavior corrections.

## Problem statement

Desktop logging run state is coordinated across start, sample, CSV, stop,
Connect and destruction handlers. Callers must know the ordering of snapshot
capture, cache initialization, CSV creation, acquisition startup and cleanup.
Modal notices and synchronous subscribers can re-enter these handlers.

Existing lifecycle tests frequently construct the entire window and inspect its
private state. Changes to run invariants lack locality, despite the backend
already owning preparation, conversion and acquisition policy.

## Solution

Introduce one concrete desktop logging run module that lives for the window's
lifetime. It owns run state and coordinates acquisition through the existing
composition-owned engine. Its interface hides lifecycle ordering and publishes
coherent read-only presentation facts and outcomes.

Depth comes from concentrating invariants, rather than forwarding existing
handlers. Removing the module would redistribute ordering and cleanup knowledge
across its callers: this is the deletion test for the proposed design.

## User stories

1. As an operator, I want a run to capture my logging selection and ECU/TCU
   target so later edits cannot change current acquisition or displays.
2. As an operator, I want pending selection edits to show that a restart is
   needed only when they differ from the active logging selection.
3. As an operator, I want unavailable readings to remain unavailable in displays
   and CSV rather than showing a previous reading as current.
4. As an operator, I want each run to create its own exclusive CSV file without
   truncating or appending to completed output.
5. As an operator, I want file logging failures to leave acquisition and
   displays running and report the CSV failure once.
6. As an operator, I want a new run or explicit CSV re-enable to retry file
   creation with the captured schema.
7. As an operator, I want disabling CSV to close output without stopping
   acquisition, and re-enabling it to create another exclusive file.
8. As an operator, I want stopping, connection changes and application shutdown
   to release logging resources before the adapter is reused or destroyed.
9. As a maintainer, I want run invariants tested through the same interface
   used by the window, without constructing widgets for lifecycle scenarios.

## Implementation decisions

- Keep the module desktop-side. It may use Qt and existing desktop adapters;
  portable backend policy gains no Qt, thread or filesystem dependencies.
- Composition retains ownership of the engine, worker execution resources and
  borrowed hardware dependencies. The module borrows the engine and stops its
  run before those dependencies are released.
- Route all desktop acquisition start/stop requests through the module,
  including Connect and window destruction. Engine internals continue to own
  worker cancellation, joining and queued-event generation fencing.
- Own active snapshot, value cache, elapsed run timing, CSV file, CSV failure
  latch and output path. CSV enablement persists between successive runs.
- Keep the pending start request and identification sequencing with the
  window's connection flow. After identification succeeds, that flow supplies
  captured run inputs. The module does not own connection decisions.
- Keep logger definitions, selection editing/persistence, idle display choices,
  widget layout, translation, OFF/ON rendering and dialogs with existing owners.
- Publish a coherent read-only view of active selection, measurement metadata
  and readings. Callers must not coordinate separately mutable snapshot/cache
  state. Exact types, notification mechanism and view lifetimes remain to be
  designed; no borrowed view may survive a reentrant transition without an
  explicit lifetime guarantee.
- Finish owned state transitions before publishing their outcomes. Synchronous
  subscribers may re-enter; do not assume that notification delivery prevents
  this. Engine startup can emit diagnostics before returning an error, and
  Stop publishes completion synchronously after joining and releasing its run.
- Preserve state initialization before acquisition startup and rollback on a
  returned startup failure. A CSV failure alone must not block acquisition.
- Preserve per-sample stop checks and healthy-peer handling after a sample
  error. Characterize replacement of a run during notification separately.
- Preserve fresh-file creation, captured columns, first sample retention and
  once-per-failure reporting. Explicit CSV re-enable and a new run retry;
  ordinary subsequent samples do not retry after a latched failure.
- Elapsed time resets per logging run, not per CSV re-enable. Shutdown closes
  output and reports close errors without displaying modal notices.
- Keep identification shutdown ordering and synchronous logging cleanup before
  connection/facade reuse. Normal cleanup publishes completion only after
  owned run state and output have been released.
- Prefer existing seams and adapters. Production protocols and scripted
  protocols already vary; CSV uses real and failing storage adapters. Do not
  invent a virtual engine or generic storage interface solely for extraction.

## Testing decisions

The module's interface is the primary test surface. Use package-owned GoogleTest
suites and existing scripted protocol and failing CSV storage adapters. Assert
observable state, notifications, readings, output and resource release rather
than private field ordering.

Transfer applicable lifecycle scenarios from window tests:

- Successful startup, invalid preparation, factory failure and handshake failure.
- Normal stop, cancellation, adapter loss and runtime failure.
- CSV open/write failure, single reporting, continued display updates and retry.
- Fresh files across runs and CSV disable/re-enable; captured column identity,
  escaping, unavailable values and first-row retention remain covered by their
  owning modules as well as relevant composition scenarios.
- Active versus pending selection, frozen target/protocol and fresh run caches.
- Connect joins acquisition and closes output before identification begins.
- Destruction releases resources without modal notices.
- Error observers stopping the run, immediate restart from completion and stale
  worker-event rejection, preserving existing engine coverage.

Retain focused window tests for actions, rendering, pending-selection indicators,
identification handoff and composition lifetimes. Avoid duplicating existing
portable conversion, column policy or engine tests merely to relocate ownership.

Before deciding a correction, characterize notification that stops run A and
starts run B while an A handler is suspended. Current desktop presence checks do
not establish run identity; engine generation fencing alone does not protect an
already-entered desktop handler. Record a demonstrated defect separately and
obtain a correction decision rather than silently including it in extraction.

## Out of scope

- Changes to protocol traffic, hardware qualification or acquisition policy.
- Logger measurement authoring, gauge rendering or live reconfiguration.
- Moving definition installation or selection persistence into this module.
- Terminal script execution or unrelated architectural cleanup.
- Automatic correction of uncharacterized reentrancy or destruction behavior.
- Final interface declarations, implementation tickets, commits or publication.

## Further notes

This design preserves [current logging decisions](../../design-notes.md#logging),
[logging contracts](../../reference/logging-contracts.md), and
[composition lifetimes](../../reference/desktop-contracts.md#composition-lifetime).
It requires no reopening of [ADR 0016](../../adr/0016-enforce-qt-reachability-by-visibility.md).
Use the [domain language](../../../CONTEXT.md) for run selection and readings.

On implementation, update the owning logging contract to describe the new
desktop ownership and link to it from other documentation. Follow the repository
documentation lifecycle for this specification after enduring knowledge has
been extracted and its original contents preserved in delivered Git history.
