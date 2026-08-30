# Desktop Quick Dashboard Visualizations — Design

**Status:** Approved 2026-08-30.

## Context

The QtQuick dashboard now provides a functional numeric-card vertical slice. It
loads a bundled, validated `.ohd` document, configures and runs a CDBG/CAN
session, coalesces live samples to a bounded UI refresh rate, renders a
responsive grid, and preserves visibly stale readings when communication stops.

The parent dashboard design also promises sparkline and horizontal-gauge cards.
The portable document and codec already represent both display types, including
sparkline history duration and conversion or card-specific gauge bounds. The
presentation layer currently rejects non-numeric cards, exposes no visualization
configuration or history roles, and always creates `NumericCard` delegates.

This design delivers the remaining visualization types as the next independent
dashboard slice. It extends the existing presentation boundary without pulling
forward editor, persistence, or packaging work.

## Goals

- Render numeric, sparkline, and horizontal-gauge cards from one validated
  mixed-type dashboard document.
- Retain the current card's prominent value, unit, state, and stale-age treatment
  for every display type.
- Maintain bounded, monotonic, session-local sparkline history at the existing UI
  refresh cadence.
- Represent sampling interruptions as visible breaks rather than misleading
  continuous lines.
- Use stable configured bounds for both visualization types.
- Keep acquisition, history policy, and session lifecycle testable outside QML.
- Preserve the responsive common card footprint and the existing numeric-card
  behavior.

## Non-goals

- Dashboard editing, document mutation, catalog import UI, or open/save actions.
- New `.ohd` fields, format-version changes, or persisted sample history.
- Qt Charts or another charting dependency.
- Arbitrary card sizes, spans, positions, or type-specific grid footprints.
- Animation, hover inspection, zooming, selectable palettes, or axis controls.
- Retaining every backend sample or introducing time-bucket aggregation.

## Chosen approach

`DashboardCardModel` owns visualization configuration and sparkline history for
each card row. `DashboardController` continues to coalesce backend samples and
passes the latest value and its receipt timestamp to the model. QML selects a
typed delegate and renders model-owned state.

This keeps session and history rules deterministic in C++, keeps QML focused on
drawing, and preserves the existing single-model boundary. A child model per
sparkline would add QObject lifecycle and model-notification complexity without
a current need. QML-owned history would couple data lifetime to delegate
creation and grid recycling, making reconnect and resize behavior fragile.

## Presentation model

Each `DashboardCardModel` row retains its existing identity and reading roles and
adds immutable roles for:

- display type;
- effective minimum, maximum, and step;
- sparkline history duration; and
- sparkline points.

Effective bounds use the card's `GaugeBoundsOverride` when present and otherwise
use the selected conversion's `gauge_min`, `gauge_max`, and `gauge_step`.
Although the portable model names these gauge bounds, this milestone treats them
as the stable display scale for both gauges and sparklines. Numeric cards ignore
the visualization roles.

Sparkline points are lightweight elapsed-time/value pairs exposed as a coherent
value snapshot. The representation is an implementation detail of the model;
QML does not receive backend objects or mutable history ownership. The model may
change the role's internal transport later if profiling demonstrates that
snapshot conversion is material.

The bundled dashboard loader stops rejecting non-numeric cards. Portable
dashboard validation remains authoritative for display types, missing history
durations, and invalid bounds. Presentation construction must nevertheless fail
safely if it receives an unsupported display type: it returns a load error
through the existing dashboard-loading path rather than exposing a partially
initialized row.

## Sample and history data flow

The existing 33 ms single-shot flush timer remains the sole presentation update
clock. It coalesces backend activity by channel and supplies the most recent
value with its actual monotonic receipt timestamp. A flush for a sparkline row:

1. updates the current formatted and numeric reading;
2. appends one timestamped history point;
3. prunes points older than that card's configured history duration; and
4. emits current-reading and history role changes as one coherent row update.

History is therefore sampled at no more than the bounded UI cadence, not at
every backend callback. Two samples with the same timestamp replace the latest
point rather than producing duplicate x coordinates. Non-finite values remain
rejected before they reach either the current reading or history.

The existing document validation restricts sparkline duration to 1–300 seconds.
At the maximum 30 Hz presentation cadence this bounds a card to approximately
9,100 points. Pruning occurs when a valid sample arrives. Preserved history does
not decay merely because a card is disconnected and not being redrawn.

The x-axis uses real elapsed time across the configured history window rather
than evenly spaced sample indexes. Consecutive points belong to different line
segments when their timestamps differ by more than three times the document's
configured CDBG sampling interval, with a minimum threshold of 100 ms. This
derived rule avoids a new format-version-1 setting while showing meaningful
communication gaps.

## Session lifecycle

Transitioning into a new `Connecting` attempt clears every sparkline history
before new session samples can arrive. This applies to initial connection and
explicit reconnect attempts.

ECU silence, failure, disconnecting, and disconnected states retain existing
history and mark the last reading stale through the current state machinery. A
valid recovered sample removes stale state. If its elapsed-time gap exceeds the
segmentation threshold, it starts a new line segment.

The history is presentation-only. It is owned with the controller/model,
discarded when that presentation is destroyed, and never copied into or saved
with the `.ohd` document.

## QML component structure

`DashboardView.qml` becomes a typed delegate dispatcher that instantiates one of
three components based on the display-type role:

- the existing `NumericCard`;
- `SparklineCard`; or
- `HorizontalGaugeCard`.

All types compose a reusable `DashboardCardFrame`. The frame owns the current
background and border treatment, title, state label, prominent current value,
unit, stale age, and accessibility name. `NumericCard` becomes a thin
composition over this frame so extracting common structure does not change its
appearance or behavior.

Every delegate keeps the same responsive-grid footprint. No type-specific
spans or dimensions are introduced.

## Sparkline rendering

`SparklineCard` uses the lower portion of the common frame for a frameless QML
Canvas plot. It has these rules:

- the vertical scale is fixed to the row's effective bounds;
- the horizontal scale covers the configured history window using real elapsed
  time;
- separate paths render timestamp segments divided by sampling gaps;
- values outside the vertical range are clipped for geometry, with a small
  marker at the corresponding edge;
- subdued reference lines use the configured step, subject to an explicit tick
  count cap;
- one or zero points produce no line while the prominent value remains usable;
- stale history remains visible but is dimmed consistently with the frame; and
- there are no axis labels or pointer interactions in this milestone.

Canvas drawing is invalidated only when its relevant roles or dimensions change.
It has no independent animation or sampling timer.

## Horizontal-gauge rendering

`HorizontalGaugeCard` uses the lower portion of the frame for a QML Canvas
track. The fill is normalized between the effective minimum and maximum. Tick
spacing derives from the effective step, but only the two endpoints receive
labels to avoid crowding.

An out-of-range reading clamps the fill at the nearest edge while leaving the
prominent numeric value uncapped. A directional marker at that edge indicates
under-range or over-range. This preserves the real reading without letting
geometry escape its track.

The waiting state shows an empty neutral track. The stale state retains and dims
the last valid fill and preserves the standard stale-age presentation.

## Accessibility and defensive rendering

All cards expose the same concise accessible name: title, current value, unit,
and reading state. The visualization is supplemental; line position, fill
length, and color are never the only representations of the current reading or
state.

Drawing code handles defensive edge cases without affecting acquisition:

- a zero-sized drawing area renders nothing;
- non-finite coordinates are discarded;
- out-of-range values are clipped only for geometry;
- tick generation has an explicit count cap; and
- a Canvas or drawing failure leaves the common frame and numeric value usable.

Visualization rendering cannot change connection state, stop acquisition, or
mutate the dashboard document.

## Testing

### Presentation model and controller

- Display-type and effective-bound roles for conversion defaults and card
  overrides.
- Sparkline-duration and point roles for each display type.
- History append, timestamp ordering, equal-timestamp replacement, and duration
  pruning.
- Independent histories for different card rows.
- Gap segmentation at, below, and above the derived threshold.
- History clearing on each transition into a new connection attempt.
- History preservation through silence, failure, disconnecting, and disconnected
  states.
- Non-finite sample rejection and role-specific `dataChanged` notifications.

### QML and application composition

- A mixed-type fixture selects exactly one correct delegate per card.
- All delegates retain prominent current value, unit, state, stale age, and
  accessible name behavior.
- Gauge normalization, endpoint labels, clamping, and overflow direction.
- Sparkline time positioning, segmentation, clipping, and insufficient-point
  states.
- Waiting, live, and stale visual inputs for both new card types.
- Responsive resize preserves the shared footprint and does not recreate or
  clear model-owned history.

The bundled Colt dashboard contains at least one card of each display type so
the executable exercises the complete path. Existing numeric-card, connection,
controller, portable dashboard validation, and desktop-quick smoke tests remain
regression gates.

## Delivery boundary

This slice changes only the existing desktop-quick presentation layer, its QML
resources, tests, and bundled dashboard fixture. It does not alter the portable
schema or Widgets application behavior.

The slice is complete when:

- a validated mixed-type `.ohd` loads without a presentation-layer numeric-only
  restriction;
- a live session updates all three card types at the existing bounded refresh
  cadence;
- sparklines use stable bounds, elapsed time, bounded history, and visible gaps;
- gauges retain real values while clamping and marking out-of-range geometry;
- a new connection attempt clears history while stale and disconnected states
  retain the last visuals;
- every visualization remains accessible without relying on color or geometry;
  and
- desktop-quick and relevant portable tests pass without changing Widgets
  behavior.

## References

- [Desktop Quick Application and Configurable CDBG Dashboard](2026-08-24-desktop-quick-dashboard-design.md)
- [Desktop Quick Functional Dashboard](2026-08-29-desktop-quick-functional-dashboard-design.md)
