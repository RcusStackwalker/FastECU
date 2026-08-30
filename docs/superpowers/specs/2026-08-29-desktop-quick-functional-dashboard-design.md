# Desktop Quick Functional Dashboard — Design

**Status:** Approved 2026-08-29.

## Context

The approved [Desktop Quick Application and Configurable CDBG Dashboard
design](2026-08-24-desktop-quick-dashboard-design.md) divides the first
OmniHaste milestone into focused checkpoints. The QtQuick shell, portable
`.ohd` model and codec, shared logging runtime, configurable CDBG session, and
desktop connection controller now provide the application, document, worker,
protocol, hardware, and connection-state foundations.

The next checkpoint delivers the first functional dashboard presentation
slice. It loads a bundled Mitsubishi Colt `.ohd`, renders its configured
numeric cards, and applies live samples from the existing logging engine. It
does not yet add document workflows, card editing, gauges, or sparklines.

## Goals

- Load and validate a realistic bundled Colt dashboard through the production
  document service.
- Show its numeric cards before connection with an explicit waiting state.
- Feed logging-engine samples into those cards without exposing backend or
  hardware objects to QML.
- Coalesce presentation updates to at most 30 Hz while retaining the newest
  value received for every channel.
- Preserve last-known readings and mark them stale during silence,
  disconnection, and failure.
- Keep card order stable in an equal-sized responsive grid.
- Keep the connection controller focused on connection workflow and the
  Widgets application behaviorally unchanged.

## Non-goals

- Opening, importing, saving, or restoring user-selected `.ohd` files.
- Dirty state or unsaved-change handling.
- Adding, removing, reordering, or configuring cards.
- Horizontal-gauge or sparkline rendering and history buffers.
- Persisting adapter selection.
- Logging-history storage or diagnostic export.
- Completing real-hardware bench qualification.

## Architecture

The composition root loads one bundled `.ohd` through
`DashboardDocumentService` and supplies copies of the validated document to
the connection and dashboard presentation controllers:

```text
bundled Colt .ohd
    -> DashboardDocumentService
         |-> DashboardConnectionController
         `-> DashboardController
              -> DashboardCardModel
                   -> DashboardView / NumericCard

LoggingEngine
    |-> DashboardConnectionController  (status and completion)
    `-> DashboardController            (sample batches)
```

`DashboardConnectionController` retains exclusive responsibility for adapter
selection, connection actions, and connection lifecycle state.
`DashboardController` owns dashboard presentation state, sample coalescing,
and the `DashboardCardModel`. The two controllers communicate only through
stable presentation state; the dashboard controller does not call connection
actions.

The logging-engine presentation interface exposes the existing
`valuesUpdated` signal in addition to status and completion signals. Neither
controller receives a protocol, transport, adapter, or writable hardware API.

The document remains immutable for this checkpoint. Each controller owns the
copy supplied at composition, and the composition test proves that both copies
are semantically equal. A later document-workflow checkpoint will introduce a
single mutable document owner and explicit replacement notifications.

## Bundled Colt document

The runtime resource is a checked-in, human-readable `.ohd` document derived
from the repository's existing Colt CDBG catalog. It contains real connection
settings, addresses, conversions, and a small useful set of numeric cards such
as engine speed and coolant temperature. It does not depend on the legacy XML
catalog at runtime.

Startup reads the resource through the existing Qt resource repository and
decodes and validates it with `DashboardDocumentService`. The application does
not construct a duplicate dashboard in C++.

The bundled document must:

- use format version 1 and the CDBG/raw-CAN profile;
- contain only numeric cards in this checkpoint;
- have unique card-to-channel mappings and contiguous document order;
- build a valid dashboard logging session; and
- use values derived from the checked-in Colt catalog rather than synthetic
  hardware addresses.

If the resource cannot be read or validated, the application loads its shell
but displays a dashboard-load error, exposes no card rows, and keeps Connect
disabled. It never substitutes a synthetic or partially parsed document.

## Presentation components

### Dashboard controller

`DashboardController` is a `QObject` presentation adapter. It receives the
validated document, the logging-engine presentation interface, and the
connection controller. It exposes a constant `QAbstractItemModel` property for
cards and read-only dashboard title and load-state properties.

The controller listens to:

- logging-engine sample batches;
- connection-controller state changes; and
- internal value-flush and age-update timers.

It owns no connection commands. QML continues to invoke Connect, adapter
selection, Refresh, and Disconnect only on `DashboardConnectionController`.

### Dashboard card model

`DashboardCardModel` creates one row per document card in ascending `order`.
It resolves the card's channel and selected conversion during initialization
and exposes explicit roles for:

- card ID and channel ID;
- display title;
- formatted value and numeric value;
- unit and decimal precision;
- reading state;
- whether a valid reading has ever been received; and
- last-update age text.

The title is the card override when present and the channel display name
otherwise. Numeric formatting uses the selected conversion's fixed decimal
precision. QML does not perform numeric formatting or resolve document
references.

The model is a presentation projection, not a document editor. It never
changes `DashboardDocument`, creates rows for incoming samples, or exposes
backend pointers.

## Sample data flow and coalescing

When a sample batch arrives on the GUI thread, `DashboardController` validates
each sample's channel ID and finite numeric value. It stores the newest valid
sample for each known channel in a pending map. Unknown channel IDs and
non-finite values are ignored and emitted through the application's diagnostic
path without changing card state.

The first pending sample arms a single-shot presentation timer. The timer
flushes at most once per 33 milliseconds, applies the latest pending sample for
each affected channel, clears the pending map, and emits compact
`dataChanged` ranges where practical. Samples arriving before the flush replace
older pending values for the same channel. Acquisition and worker polling are
never delayed by rendering.

The 30 Hz limit applies to value-model notifications, not connection-state
notifications. A status transition is visible immediately. If a session ends
with pending samples, the controller applies the newest pending values before
marking cards stale so the last valid readings are not lost.

Sample age is measured from GUI-thread receipt with a monotonic clock. A
separate one-second timer updates the age role only for rows that have received
a value. Wall-clock changes do not affect age. The timer stops when no received
row requires an age update.

## Reading lifecycle

Every row has one of three presentation states:

```text
Waiting -> Live -> Stale -> Live
```

- **Waiting:** the card has never received a valid sample. It displays an em
  dash and `Waiting for data`.
- **Live:** the card has received a valid sample and the connection controller
  reports `Running`.
- **Stale:** the card retains a prior value while the connection is
  `CarNotResponding`, `Disconnecting`, `Disconnected`, or `Failed`.

Entering `Connecting` or `AdapterSelectionRequired` does not make an old value
live and does not erase it. A card becomes live only when a valid sample is
received during the active run. A partial batch updates only referenced cards;
other rows retain their previous value and state. A transition to
`CarNotResponding` marks all previously received rows stale immediately. The
next valid sample returns its corresponding row to live, while the connection
controller's subsequent `Running` state permits other newly sampled rows to
become live.

An explicit disconnect, adapter loss, or runtime failure preserves readings.
No loss-of-communication path writes zero or an empty value over the last valid
sample.

## QML layout and behavior

The workspace replaces its placeholder content with three stable regions:

1. a header containing the bundled dashboard name and overall connection
   status;
2. a scrollable card area that consumes the remaining height; and
3. the existing connection panel at the bottom.

`DashboardView.qml` uses a `GridLayout` whose column count is calculated from
the available width and a shared minimum card width:

```text
columns = max(1, floor(available width / minimum card width))
```

All cards have equal width within a row and a consistent minimum height. They
flow left to right and then top to bottom in document order. Window resizing
changes only the calculated column count; it neither changes model order nor
writes layout information into the document.

`NumericCard.qml` is a passive, role-driven component. It shows the title, a
large right-aligned formatted value, the unit, a text-and-color state
indicator, and last-update age for stale values. Waiting cards use subdued text
and an em dash. Live cards use the normal high-contrast theme. Stale cards dim
the retained value and use an amber indicator; a connection failure does not
turn the complete grid red.

Accessible names combine title, formatted value, unit, and reading state.
Waiting and stale states are communicated through text as well as color.

The bundled resource containing a non-numeric card is rejected during startup.
The UI does not silently render a declared gauge or sparkline as numeric.

## Error handling

Presentation failures follow these rules:

| Condition | Behavior |
|---|---|
| Bundled resource missing or unreadable | Show dashboard-load error, expose no cards, disable Connect. |
| Document invalid or unsupported | Show the stable field path and concise detail; do not partially load it. |
| Unsupported card display type | Reject the fixture as invalid for this checkpoint. |
| Unknown sample channel | Ignore the sample and publish a diagnostic. |
| Non-finite sample value | Ignore the sample and publish a diagnostic. |
| ECU silence | Retain readings, mark received rows stale, and keep connection recovery available. |
| Disconnect or terminal runtime error | Flush the latest valid pending values, retain them as stale, and preserve existing connection recovery controls. |

Diagnostics are not shown inside individual cards. The connection panel
continues to own concise connection failure and expandable technical detail.

## Testing

### Card model

- Document-order projection and title fallback.
- All role values and fixed-precision formatting.
- Waiting, live, stale, and resumed transitions.
- Retained readings after silence, disconnect, and failure.
- Partial batches and independent row state.
- Unknown-channel and non-finite sample rejection.
- Compact model notifications without row insertion or reset during streaming.

### Dashboard controller

- Newest-value-wins reduction within one coalescing interval.
- No more than one value flush per 33 milliseconds.
- Immediate connection-state transitions independent of the value timer.
- Pending-value flush before terminal stale transition.
- Monotonic age calculation and one-second age notifications.
- Reconnect behavior that leaves old values stale until fresh samples arrive.
- Safe destruction with armed timers and queued samples.

Timing tests use injected timer/clock seams or deterministic event-loop control;
they do not rely on long wall-clock sleeps.

### QML and composition

- The complete root component loads offscreen with fake services.
- The bundled dashboard title and expected numeric cards appear.
- Card roles bind to waiting, live, and stale visual states.
- Column count changes responsively without changing card order.
- Accessible names include value, unit, and state.
- Existing adapter selection and connection controls continue to work.
- The runtime `.ohd` is loaded through the real codec and supplied
  semantically unchanged to both controllers.
- Missing and invalid resource fixtures produce a visible non-connectable
  application state.

### Regression gates

- Validate the checked-in Colt `.ohd` and build a session from it.
- Run focused dashboard, logging-engine, connection-controller, and application
  tests.
- Build `//:fastecu` and `//:fastecu-desktop-quick`.
- Run `//:portable_closure`, repository formatting, and diff checks.

The real CDBG/CAN path remains available for manual use, but a recorded bench
run is not required until the hardening and qualification checkpoint.

## Acceptance criteria

This checkpoint is complete when:

- OmniHaste loads a valid bundled Colt `.ohd` through the production document
  service at startup.
- The document's numeric cards are visible before connection in document order
  with waiting placeholders.
- A real or fake-backed CDBG run updates matching cards while value-model
  notifications remain bounded to 30 Hz.
- Each card displays its configured title, precision, and unit.
- Silence, disconnection, and runtime failure retain and visibly stale the last
  valid readings, including their monotonic age.
- Unknown or invalid samples cannot add rows or corrupt displayed values.
- Resizing the window changes the equal-card column count without changing
  document order.
- QML has no access to transport, adapter, protocol, or ECU write APIs.
- Existing connection-controller behavior and both desktop application builds
  remain green.

## Delivery boundary

This design covers one implementation plan: bundled-document composition,
dashboard presentation controller and model, numeric-card QML, live-value
coalescing, stale-state behavior, and focused regression gates. The following
checkpoint begins only after this slice is complete and adds horizontal gauges,
bounded sparklines, and their responsive presentation behavior.

## References

- [Desktop Quick Application and Configurable CDBG Dashboard
  design](2026-08-24-desktop-quick-dashboard-design.md)
- [Desktop Quick Connection Service and Controller
  design](2026-08-26-desktop-quick-connection-controller-design.md)
- [Desktop Quick CDBG Session Foundation
  design](2026-08-26-desktop-quick-cdbg-session-foundation-design.md)
