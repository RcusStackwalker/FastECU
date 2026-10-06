# Calibration typed data flow

Status: design approved by the maintainer on 2026-10-06. The typed-flow change
and ownership extraction remain separate deliveries. The
[implementation plan](../plans/2026-10-06-calibration-typed-data-flow.md) defines
the first change for execution review.

## Purpose and delivery boundary

Replace calibration's internal comma-separated value transport with typed data.
Numeric calculations use `double`; decimal formatting belongs to presentation.
Legacy text-rounding stages and byte-for-byte edit outcomes are not compatibility
requirements for this redesign.

Deliver the typed flow and migrate its consumers in the first change. Extract
remaining field-resolution, patch-application, and selectable-write policy into
backend ownership in a separate follow-up. UI identity capture, dialogs, operator
interaction, and refresh remain with their desktop owners.

The [technical debt roadmap](../../tech-debt.md#p1-separate-ui-from-application-logic)
owns the unresolved action. The [calibration reference](../../reference/calibration-compatibility.md)
describes current behavior; this proposal deliberately supersedes selected edit
behaviors only when implemented. [Domain terms](../../../CONTEXT.md) distinguish
scaled values, raw values, invalid cells, and structural map failures.

## Current evidence

- [Calibration decoding](../../../src/backend/calibration/calibration_service.cpp)
  formats numeric cells and concatenates a comma after each value.
- [Presentation](../../../src/ui/desktop/calibration/map_presentation.cpp) and the
  [edit adapter](../../../src/ui/desktop/calibration/map_edit_adapter.cpp) split
  those strings. Patch application decodes again to derive an index limit from
  split length, including the trailing empty entry.
- [Edit calculations](../../../src/backend/calibration/map_edit.cpp) parse cell
  text into floats and format numeric inputs again before encoding. Increment
  can retry a requested step until stored bytes change.
- The [expression evaluator](../../../src/algorithms/expression/expression_evaluator.cpp)
  formats intermediate results, changes NaN to zero, and also serves logging.
- [Widget tests](../../../src/ui/desktop/widgets/calibration_maps_test.cpp) include
  static labels such as `Low` and `High`. Blob bodies are byte sequences rather
  than numeric grids. A homogeneous collection of doubles cannot cover all maps.
- Selectable write encoding is in
  [MainWindow](../../../src/ui/desktop/widgets/mainwindow.cpp), rather than the
  map-edit adapter. Its ownership extraction belongs to the follow-up.

## Typed model and data flow

Decoded maps are owned, transient snapshots of the target session's current ROM.
ROM bytes remain authoritative; there is no parallel mutable value cache.

The model distinguishes numeric body cells from blob bytes. Each numeric cell
contains a finite scaled `double` or a diagnostic explaining computation failure.
Axes explicitly distinguish absence, numeric cells, and static labels. Text labels
remain text; absent axes have no fabricated blank cell. Labels are not parsed as
numeric values. Blob selection presentation compares byte identity and formats
hex only where needed for display.

The flow is ROM bytes, typed decoding, typed edit calculation, encoded patch,
validated commit, and fresh typed decoding for presentation. Grid strings are
outputs of presentation. User-entered expressions and pasted text are external
inputs parsed at the boundary, not an internal value transport.

Selection bounds use the actual target extent. A four-cell target has indices
zero through three; there is no fifth cell created by a trailing delimiter.
Patch checks validate target indices, intended target addresses, and every byte
range before mutation. Existing address-mapping rules remain authoritative.

Stable session and map identity select the target. After modal input, use that
session's current bytes when calculating the edit; a closed session cannot be
replaced by another session through renumbering.

## Numeric evaluation and encoding

Calibration uses a checked numeric expression API with `double` input and output.
Supported syntax is `x`, decimal/scientific-notation literals, whitespace,
`+`, `-`, `*`, `/`, parentheses, and unary signs. Missing or blank expressions
mean identity. Absent numeric scaling also means identity; an unresolved named
scaling remains a definition error.

Malformed syntax, division by zero, and non-finite inputs or results are errors.
Intermediate numeric results are not formatted or decimal-rounded. Keep the
existing logging-facing evaluator APIs and behavior in this change. The checked
API belongs in algorithms and uses an algorithm-owned or standard error result;
calibration translates it into existing backend errors with map/cell context.
No new backend `ErrorKind` is required.

For each requested scaled value:

1. Validate finite numeric inputs and definition metadata. Malformed limits,
   non-finite limits, or minimum greater than maximum reject the affected edit.
2. Clamp to declared minimum/maximum bounds. Missing bounds add no restriction.
3. Evaluate the encoding expression using `double`.
4. For integer storage, round to nearest with exact halves away from zero.
   Check exact signed/unsigned storage limits before converting or narrowing.
5. For float storage, convert to its declared representation without integer
   rounding. Reject overflow; ordinary representational rounding is inherent.
6. Pack bytes using the existing layout, striding, and byte-order rules.

Display follows the definition's format. Two decimal places are typical, not a
global cap. Display formatting never feeds back into edits.

Set Value accepts signed literals as absolute assignments (`-20` assigns negative
twenty). Relative edits explicitly use `x` (`x-20` subtracts twenty); leading
operator shortcuts do not override the numeric expression grammar.

## Edit rules

An invalid numeric cell can receive direct assignment or pasted replacement if
its storage location and encoding are valid. These operations do not require its
old scaled value. A broken decoding expression may leave the cell invalid after
a successful assignment; presentation always shows the fresh decode outcome.

Increment/decrement and expressions relative to the current value require a valid
starting value. Apply exactly one requested increment, without retry accumulation
or hidden fractional state. Unchanged encoded cells are no-ops. If no stored value
changes, report that outcome and identify sub-resolution increments when applicable;
clamping can also cause no change.

Interpolation requires valid endpoints or corners. Invalid interior cells can
be replaced because their old values do not determine the interpolation result.

Every edit is atomic across the selected cells. Calculate and validate all writes
before committing. Invalid required inputs, metadata, encoding, target indices,
or byte ranges reject the whole edit with cell context. Rejection preserves ROM
bytes and dirty state. A complete no-op does not mark a clean session dirty.

## Error presentation

Invalid numeric cells display `NaN`, with the diagnostic available on hover.
The diagnostic is retained separately from the numeric value. Invalid cells do
not contribute to numeric color bounds; if no valid numeric values exist, use a
neutral presentation without inventing a numeric range.

Missing addresses, invalid dimensions, and ranges outside the ROM are structural
map failures. Keep the map window open with a clear error and disable grid edits.
Other maps remain usable. A failed refresh replaces the affected values or grid
with its error state rather than silently preserving stale rendering.

## Scope and exclusions

The first change covers typed decode results, checked calibration expressions,
session decode consumers, edit calculations, exact patch bounds/atomic validation,
and grid presentation. Existing selection behavior and definition-less sessions
remain modeled. Blob/static-axis consumers migrate to explicit typed data;
selectable-write policy extraction remains separate.

Do not change the evidence-gated `wrx02` read/write predicate, existing endian
rules, flash/checksum workflows, logger behavior, definition-authoring formats,
or switch/MultiSelectable semantics as incidental cleanup. Do not widen Bazel
layer visibility or introduce Qt into algorithms/backend.

## Validation and document reconciliation

Use package-owned GoogleTest suites in algorithms/expression, backend/calibration,
backend/calibration/session, UI calibration adapters, and calibration widgets.
Cover expression grammar/errors and full-double evaluation; typed labels/blobs
and absent axes; per-cell versus structural failures; all supported integer widths
and float overflow; rounding ties; clamping; invalid-cell replacement; atomic
failure; interpolation inputs; no-op increments/dirty state; exact selection
bounds; stable session identity; and error presentation after refresh.

Replace tests pinning trailing-empty-entry acceptance, silent invalid-to-zero
conversion, float/text precision stages, and increment retry accumulation with
tests for the agreed behavior. Preserve tests of the logging-facing evaluator
and run logging conversion/session regressions because expression code is shared.

Run applicable full Bazel, formatting, Gazelle, and static-analysis gates when
implementing. Documentation-only review uses Markdown link checks and scoped
diff review. This design provides no hardware qualification evidence.

At implementation, reconcile changed behavior in the owning calibration reference
and design notes, reduce debt only for delivered work, and preserve this proposal
in delivered Git history before removing it under the documentation lifecycle.
