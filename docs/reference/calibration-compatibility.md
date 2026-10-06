# Calibration compatibility

Current contracts for calibration data and operator sequencing. Decision
rationale is in the [calibration design notes](../design-notes.md#calibration).

## Older edited files

Files edited before PR #274 on signed multi-byte, int24, or float-storage maps
may contain bytes that differ from what the grid displayed. Those builds
byte-swapped signed reads, read signed 24-bit values as zero, inverted write
endianness, and stored integer-parse bits for float edits. Current builds decode
the actual bytes correctly. Recheck such files against their definitions before
flashing them; decoding correctly cannot recover an earlier intended edit.

Current edits respect `startpos`/`interval` striding and declared byte order;
float storage uses IEEE-754 bits. Float storage remains big-endian regardless
of the endian label. The unresolved `wrx02` read/write
address-predicate mismatch is owned by [technical debt](../tech-debt.md).

## Session and view ownership

[CalibrationWorkspace](../../src/backend/calibration/session/calibration_workspace.h)
belongs to desktop composition, which outlives its windows. Session IDs are
stable and never reused within a workspace. Closing one ROM does not renumber
another; stale IDs resolve to nothing. Closing a session removes its map windows
and view state together, including color ranges. Expanded categories, open maps,
and missing-definition placeholders are UI state separate from ROM data.

A session owns its ROM bytes, optional resolved definition, and protocol
metadata. ROM bytes are the truth for values: map windows decode on demand,
edits write bytes, and views re-decode. Map metadata is read from the typed
definition. Definition-less ROMs remain modeled sessions; hex display owns a
snapshot instead of borrowing calibration data.

## Decoded values and map edits

[Decoded maps](../../src/backend/calibration/decoded_map.h) are transient owned
snapshots. Numeric cells hold finite scaled `double` values or individual errors;
blob bodies hold bytes; axes distinguish absence, numeric cells, and static
labels. Static labels containing commas remain single labels. Numeric data is
never joined into text and split for downstream calculation. Body storage/endian
use map fields before scaling defaults; axes use resolved axis fields.

[Checked expressions](../../src/algorithms/expression/checked_expression.h)
calculate with `double` throughout. Supported syntax is `x`, decimal and
scientific-notation literals, binary `+ - * /`, parentheses, and unary signs.
Missing numeric scaling and missing/blank definition expressions mean identity;
unresolved named scaling, malformed syntax, division by zero, and non-finite
results are errors. Parenthesis nesting beyond 128 levels is rejected with a
diagnostic. The logging-facing evaluator retains its separate behavior.

Decimal precision follows the definition's display format, typically up to two
places; it does not round values used for calculation. The Set Value dialog treats
signed literals as absolute assignments (`-20` assigns negative twenty) and uses
`x` explicitly for relative edits (`x-20` subtracts twenty). Direct assignments
and pasted replacements can overwrite invalid current cells if encoding and
storage are valid. Relative operations require valid current values.

[Numeric edits](../../src/backend/calibration/numeric_map_edit.h) clamp finite
requested values to valid definition minimum/maximum bounds before applying the
encoding expression. Malformed/non-finite bounds or minimum greater than maximum
reject the edit. Integer encoding rounds to nearest with exact halves away from
zero and validates exact signed/unsigned limits before narrowing. Float encoding
converts to its declared representation without integer rounding and rejects
overflow. Legacy float/text round trips and byte-for-byte edit results are not
compatibility requirements.

Every selected edit is atomic: invalid required inputs or unencodable results
reject the whole operation before changing ROM bytes or dirty state. Patch
application validates actual target indices, expected write addresses, byte
widths, and all ROM ranges before mutation. A four-cell run has exactly four
indices; a trailing delimiter cannot create a fifth. Complete no-ops preserve
dirty state and refresh the views from current bytes. Set Value refreshes its
original map window even if another window becomes active during the dialog.

The [numeric edit use case](../../src/backend/calibration/session/numeric_edit_use_case.h)
owns increment/decrement, Set Value, interpolation, and Paste for a stable
session ID, a semantic target (map body, X axis, or Y axis), and an element
range. It resolves the session, definition fields, and fresh decoded values,
then calculates, validates, and writes in one synchronous call. Closed sessions,
definition-less sessions, and unavailable numeric targets return not-applicable
outcomes, which desktop handles silently. Desktop translates table selections
and clipboard text, owns dialogs, warnings, and no-change messages, and
refreshes the originating map window. Paste validates every supplied text cell,
including cells clipped away, before clipping to the target run's edges.

Each increment applies its requested step once, with no retry accumulation or
hidden fractional state. Unchanged encoded cells are no-ops; the UI reports a
complete no-change outcome with its actual cause, distinguishing definition
limits from storage resolution. The former sign-wrap heuristic no longer
reverts valid signed increments across zero. Interpolation uses the required
endpoints for each horizontal/vertical line or corners for bidirectional mode;
invalid interior cells may be replaced. Paste retains its existing edge clipping
and ragged-row behavior. A terminal LF or CRLF delimits the last pasted row;
it does not add an empty numeric row. Interior empty cells reject the edit.

Invalid numeric cells display `NaN` with a diagnostic on hover; they contribute
no numeric color bound. Entirely invalid maps use neutral colors. A successful
assignment may still display `NaN` when the decoding expression itself is broken:
the requested value is never substituted for a fresh decode. Structural map
failures keep the window open in an explicit error state with grid editing
disabled. Failed refreshes clear stale values; unrelated maps remain usable.

Field resolution and patch application remain in the desktop edit adapter, and
selectable-write policy remains in `MainWindow`; their ownership extraction is
separate [unresolved work](../tech-debt.md#p1-separate-ui-from-application-logic).

## Open, save, and ECU-read outcomes

Checksum correction uses a temporary operation image. Save or flash receives
that image; success, failure, and cancellation leave editable session bytes
unchanged. A successful save updates source path and basename (`default.bin`
when absent), preserves file/ECU-read origin, and clears dirty. Failure preserves
source and dirty state and emits the existing log and operator notice. A later
edit marks dirty again. Warnings and confirmations belong to the UI.

ROM opening preserves primary/secondary definition precedence, reported ROM-ID
fallback, vehicle selection, the unpadded size label, and subsequent flash-method
padding. Size-validation failure keeps a header with no maps. An unreadable
indexed definition opens without a definition and retains its load-failure
notice. Authoring a definition writes it without reopening the existing
definition-less session. Failed or cancelled ECU reads create no session;
successful reads with data are adopted after dispatch completes.

See the [session API](../../src/backend/calibration/session/calibration_session.h)
and [ROM save use case](../../src/backend/calibration/session/rom_save.h).

## Preflight and correction cancellation

[CalibrationOperationCoordinator](../../src/ui/desktop/calibration/calibration_operation_coordinator.h)
owns UI sequencing through `ICalibrationInteraction`. `MainWindow` supplies the
session and retains port checks, battery polling, cleanup, and flash dispatch.

- Cancelling the missing-checksum-module warning before Write or TestWrite
  (`ChecksumSupport::Missing`, formerly `n/a`) stops the operation before
  refresh, correction, or dispatch. The cleanup guard still runs.
- Declining checksum correction, an unknown MCU, or an outcome without bytes
  leaves the operation image unchanged and allows save/write to continue.
  The "Checksum calculation canceled!" log belongs only to the declined
  missing-module case.
- Save As corrects before opening its picker. Dismissing the picker or choosing
  an empty filename writes nothing. Success relabels the session resolved
  before the picker opened, even if selection changed while it was open.

[Coordinator tests](../../src/ui/desktop/calibration/calibration_operation_coordinator_test.cpp)
cover sequencing and persistence; widget tests cover rendering and wiring.
Post-read adoption and checksum/save/write still require bench re-verification
before release; automated coverage does not establish hardware qualification.
The checksum dialog's expected presentation is recorded in its
[bench notes](../checklists/checksum-dialog-bench-notes.md).
