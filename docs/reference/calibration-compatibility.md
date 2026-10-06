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
float storage uses IEEE-754 bits. `encode_guarded` enforces the range only;
`apply_increment` also retains the sign-wrap heuristic that reverts a single
cell rather than rejecting an entire edit. The unresolved `wrx02` read/write
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
