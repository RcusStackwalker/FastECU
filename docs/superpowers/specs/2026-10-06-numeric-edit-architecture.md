# Portable numeric calibration edit use case

Status: design confirmed on 2026-10-06; implementation pending. This document
records the agreed design and does not authorize implementation.

## Agreed scope

The priority is reusable architecture under
[UI/application separation](../../tech-debt.md#p1-separate-ui-from-application-logic).
The first delivery covers numeric map edits; selectable values are deferred.

Provide a complete numeric edit use case callable without Qt and use it from
desktop. Portable tests must cover session targeting, validation, atomic mutation,
and no-op outcomes. A second consumer, including Android, is not required for this
delivery. Moving helpers alone does not satisfy the agreed scope.

Reuse the existing portable numeric calculations. Preserve the
[calibration contracts](../../reference/calibration-compatibility.md#decoded-values-and-map-edits),
including typed values, atomic selected edits, dirty-state preservation on complete
no-ops, and refresh from ROM bytes. Existing
[session and view ownership](../../reference/calibration-compatibility.md#session-and-view-ownership)
remains the design baseline.

Requests identify a semantic target (map body, X axis, or Y axis) and a range of
elements. Desktop translates table/header coordinates into that representation;
backend validates the target and bounds. Other consumers need not reproduce
desktop table conventions.

The use case accepts a stable session ID and resolves it through the workspace
at execution time. Field resolution, fresh decoding, calculation, patch
validation, and atomic mutation happen in one synchronous call. Callers retain
execution-context ownership. A stale ID returns a structured not-applicable
outcome without mutation; desktop preserves its current silent handling when the
original session has closed. Desktop also checks the original map window's
lifetime and owns its refresh.

Session references and borrowed definition-field views do not survive operator
dialogs. Set Value retains the original session/map identity, obtains the
original window's selection after acceptance, and calculates against current ROM
bytes, preserving the existing dialog behavior.

The use case returns structured outcomes: changed, or unchanged with the existing
no-change reason (definition limit, storage resolution, both, or ordinary
unchanged values). A closed session, missing definition, or unavailable numeric
target returns not-applicable with a structured reason; desktop handles it
silently. Invalid expressions, bounds, or encodings use existing typed errors.
Calculated patches remain internal; mutation finishes before return. Desktop owns
outcome messages and refreshes the original view after a change or successful no-op.

## Desktop invocation

Existing increment/decrement, Set Value, interpolation, and Paste actions invoke
the use case. Desktop obtains the originating session ID and map identity,
translates the current selection into a semantic target and element range,
collects the operation payload, and calls synchronously. Dialog cancellation or
loss of the original map window remains a desktop early return; Set Value reads
the original window's selection after dialog acceptance.

Desktop checks the returned result with `.has_value()`. An invalid edit presents
the existing warning. Changed outcomes refresh the originating view; unchanged
outcomes refresh it and present the existing no-change reason. Not-applicable
outcomes return silently. Backend does not emit widget signals, select an active
window, or construct operator dialogs.

## Deferred copy/paste scenario

The operator must be able to select all cells in a larger map, copy them, and
paste into a smaller map, applying only the cells that fit the destination.
The full source row/column layout must reach the backend so it can apply
destination-bound clipping. Copy-all for this scenario means the numeric map
body, excluding axis values and labels; axis copying is a separate operation.
Copy should preserve full-precision scaled values rather than rounded display
text. Body-only Select All, ordered Copy, full-precision serialization, and the
end-to-end larger-to-smaller workflow test are deferred to the owning
[technical-debt item](../../tech-debt.md#p2-make-full-body-map-copypaste-reliable).
The current extraction preserves existing Copy behavior and verifies portable
paste clipping; it does not claim to establish the complete copy/paste workflow.

## Paste input

Desktop reads clipboard text and splits it into owned rows of text cells.
Backend converts and validates those cells, resolves destination geometry,
clips, encodes, and applies atomically. Desktop preserves empty cells and ragged
row lengths, including the existing removal of exactly one terminal LF row,
without clipping source input. CRLF remains accepted through numeric whitespace
handling. Paste retains locale-independent dot-decimal parsing; Set Value
retains its existing comma-to-dot normalization at the desktop boundary.

The current paste calculation starts at the destination selection's top-left
and clips to the target run's edges rather than the selection's extent. Current
desktop parsing validates all supplied cells before that clipping. Invalid
source values are not a deciding concern for the intended workflow, so retain
that validation order in backend rather than introduce a behavior change.
Invalid cells, including clipped-away cells, reject the complete operation
before mutation.

## Ownership and verification

The complete use case belongs with the portable calibration sessions and
workspace; existing pure numeric primitives remain in backend calibration.
Desktop composition supplies the workspace and desktop adapters supply semantic
selections and operation inputs. Existing layer visibility suffices; no new port,
owned execution context, or platform dependency is required.

Package-owned portable GoogleTests cover all numeric operations, semantic body
and axis validation, selection bounds, stale IDs, definition-less sessions,
atomic failures, dirty-state/no-op behavior, field fallback, and complete
text-cell validation before paste clipping. Desktop tests retain original-target
identity, fresh bytes and selection after dialogs, closed-target silence, and
original-map refresh. Copy-side workflow coverage is deferred as recorded above.

Implementation follows the [testing and analysis conventions](../../coding-style.md)
and [repository build and verification gates](../../../AGENTS.md#build-and-verify),
including regenerated managed BUILD files and applicable platform CI. This
documentation-only interview uses the Markdown link gate and scoped diff review.

Preserve the evidence-gated
[wrx02 predicates](../../tech-debt.md#p1-resolve-the-wrx02-address-predicate),
existing numeric encoding behavior, and operator-dialog timing. This work
introduces no new error kind or concurrency model.

The agreed meaning of map body is recorded in the
[domain glossary](../../../CONTEXT.md). No separate ADR is introduced yet;
the proposed ownership extraction extends existing calibration decisions.
