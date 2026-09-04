# Step 6b — Calibration map-edit use case — Design

**Status:** complete. Designed 2026-09-03; four PRs landed (#271, #272, #274,
#275) plus a follow-on session fixing spec defects (b) and (c), closed out in
PR 6b-4 (pending). Spec defect (a) is deliberately deferred — see its
[status note](#a-the-wrx02-address-fixup-differs-between-read-and-write). The
second slice of step 6 of the
[modularization plan](../../modularization-plan.md), scoped — like
[6a](2026-09-01-step6a-file-actions-dewidget-design.md) — to run **in parallel
with the [per-family flash drain](2026-08-08-step5-tail-flash-drain-design.md)**
(waves 5-7, 14 families remaining) without contending for a single file.

**Goal:** move the calibration map-*edit* arithmetic out of
`src/ui/desktop/menu_actions.cpp` into portable operations under
`src/backend/calibration`, leaving the UI with selection extraction, prompting,
and repaint. This is the mirror of what 5d-4/5d-4b already did for the *decode*
side in `calibration_service`.

**Non-goal:** converting `FileActions::EcuCalDefStructure` away from
`QString`/`QStringList`. That remains the deferred item in the
[tech-debt roadmap](../../tech-debt.md) under "Replace parallel-list data
models", excluded here for the same reason 6a excluded it: it rewrites
drain-owned files.

**Also non-goal:** undo/redo. The `Undo`/`Redo` menu commands stay `qDebug()`
stubs. The edit operations return a patch rather than mutating in place, which
makes undo cheap to add later, but adding it is not this slice.

## Why this slice is independent

Measured against `master` at `6dc8c5f7` on 2026-09-03.

The slice touches `src/ui/desktop/menu_actions.cpp`, the private-method block of
`src/ui/desktop/mainwindow.h`, `src/backend/calibration/**`,
`src/algorithms/menu/**`, and the two portable-closure registration lists. It
touches no file under `src/platform/desktop/common/flash/legacy/` or
`src/ui/desktop/flash/`, and nothing inside `start_ecu_operations`
(`mainwindow.cpp:1028-1424`) or the flash-dialog include block
(`mainwindow.h:58-80`) — the two regions the drain edits.

The drain has never touched `menu_actions.cpp`. Wave 4 (`ea633a4`), the most
recent, touched `src/ui/desktop/flash/ecu/BUILD.bazel`, `mainwindow.cpp` (20
lines, all dispatch), and `mainwindow.h` (4 lines, all includes). 6a-4 already
established that `menu_actions.cpp` can be rewritten without a drain conflict.

`mainwindow.h` is edited by both, but in disjoint regions: the drain deletes
include lines at 58-80; this step rewrites the five private map-edit method
declarations at 356-363.

## What is there today

`menu_actions.cpp` is 2138 lines. About 830 are map-edit code.

| Function | Lines | Disposition |
|---|---|---|
| `menu_action_triggered` | 7-142 | Stays. UI dispatch. |
| `inc_dec_value` | 144-436 | Axis resolution + per-cell edit loop → extracted |
| `set_value` | 438-645 | Same axis-resolution block, second copy → extracted |
| `interpolate_value` | 647-887 | Same block, third copy → extracted |
| `copy_value` | 889-924 | Stays. Pure clipboard/widget. |
| `paste_value` | 926-1011 | Extracted. Body-only; no axis support. |
| `connect_to_ecu` … `winols_csv_to_romraider_xml` | 1013-1459 | Stays. Not this slice. |
| `set_maptablewidget_items` | 1460-1609 | Stays. Table repaint. |
| `get_rom_data_value` | 1610-1697 | Extracted. Pure byte codec, no widgets. |
| `set_rom_data_value` | 1699-1753 | Extracted. Pure byte codec, no widgets. |
| `get_mapvalue_decimal_count` | 1755-1765 | Extracted. Trivial string logic. |
| `get_map_cell_colors` | 1767-1803 | Scale computation extracted; `QColor` stays. |
| `check_rom_data_value` | 1805-1850 | **Deleted.** Dead. |
| `test_haltech_ic7_display`, `simulate_obd` | 1852-2138 | Stays. Misplaced, but P2 and not this slice. |

`check_rom_data_value` returns `false` unconditionally from three empty
branches, and a tree-wide grep finds only its declaration
(`mainwindow.h:362`) and its definition. It is the abandoned attempt at the
bounds unification that [defect c](#c-bounds-enforcement-differs-per-operation)
describes.

### The duplication

`inc_dec_value`, `set_value`, and `interpolate_value` each open with the same
~50-line block: read the active `QMdiSubWindow`'s object name for
`(rom_number, map_number)`, read the selected range, then branch three ways —
map body, X axis, or Y axis — swapping eleven parallel-list field reads and
adjusting the row/column offsets. The three copies are identical except that
`inc_dec_value` also swaps `CoarseIncList`/`FineIncList`, which the other two
do not read. **`inc_dec_value` is therefore the canonical copy**: it is the
only one that reads every field the rule governs.

`paste_value` does not have the block at all. It reads `MapData`,
`StorageTypeList`, `ToByteList`, `EndianList`, and `AddressList` directly, so a
paste onto a selected axis writes into the map body.

## The portable surface

A new sibling target `//src/backend/calibration:map_edit`
(`map_edit.{h,cpp}`), not an extension of `calibration_service.cpp` — that file
is 417 lines with a 911-line test already, and the edit side deserves its own
test file. Same package, so it sits next to the decode side it inverts.

Three pieces. All follow the `ElementRun` precedent from
`calibration_service.h:82-99`: a non-owning value view built at the call
boundary, so the Qt-typed model stays where it is and 6a's non-goal is not
reopened.

### `MapElementSpec`

The fields the edit path needs for one run of elements — address, storage type,
endian, `to_byte`, `from_byte`, min, max, coarse and fine increments, and the
run's dimensions. `std::string_view` fields borrow from the
`EcuCalDefStructure` lists that outlive the call, exactly as `ElementRun` does.

The UI plucks these from whichever parallel-list group `resolve_edit_target`
names. That plucking is the one place `QString` → `std::string_view`
conversion happens, and it exists once instead of four times.

### `resolve_edit_target`

```cpp
enum class EditTargetKind { MapBody, XAxis, YAxis, Rejected };

struct EditTarget
{
    EditTargetKind kind{EditTargetKind::MapBody};
    SelectionRange range;   // offsets already adjusted for the branch
    std::uint32_t x_size{1}; // 1 for YAxis, matching the legacy override
};

EditTarget resolve_edit_target(const SelectionRange& selection, MapDimensions dims,
                              std::string_view x_scale_type);
```

The duplicated three-way rule, extracted once as pure logic: a selection whose
left column is 0 on a map with `y_size > 1` targets the Y axis; one whose top
row is 0 on a map with `x_size > 1` targets the X axis; a static scale type
(`"Static Y Axis"` or `"Static X Axis"`) rejects the edit outright, which is the
bare `return` in all three legacy copies. The offset adjustments each branch
makes — and the `x_size = 1` override in the Y-axis branch — move with it.

### The edit operations

```cpp
struct CellPatch
{
    std::uint32_t index{0};      // element index within the run
    std::string display_text;    // replaces one entry of the split cell text
    std::uint64_t byte_address{0};
    std::vector<std::uint8_t> bytes;
};
using EditPatch = std::vector<CellPatch>;

Result<EditPatch> apply_increment(bytes::ByteView rom_data, const MapElementSpec& spec,
                                  std::span<const std::string_view> cell_text,
                                  const SelectionRange& range, IncrementStep step, int float_precision);
Result<EditPatch> apply_set_expression(...);
Result<EditPatch> apply_interpolation(..., InterpolationMode mode);
Result<EditPatch> apply_paste(..., std::span<const std::string_view> pasted_rows);
```

`IncrementStep` is coarse or fine, positive or negative — the four
`MenuCommand` values `FineIncrement`, `FineDecrement`, `CoarseIncrement`,
`CoarseDecrement` collapse onto it. `InterpolationMode` is horizontal,
vertical, or bidirectional. `SelectionRange` is a first/last row and column
pair; `MapDimensions` is an x/y size pair.

Returning a patch rather than mutating is what makes these testable without a
ROM buffer to inspect afterwards, and what leaves the door open for undo. The
caller applies the patch to `FullRomData` and to the split cell text, then
rejoins and repaints — the same three closing statements the legacy functions
already share.

**A failed operation produces no patch at all.** Each `apply_*` returns
`Result<EditPatch>`, and any per-cell failure — an out-of-range window, a
division by zero, an increment of zero — fails the whole call rather than
returning a partially applied patch. A half-edited map is worse than an
unedited one, and the legacy code's own behavior here is incoherent: it warns
mid-loop and keeps writing. This is the one place the extraction chooses a
defined behavior over the legacy one, and it is why the warning conditions
become error kinds rather than side effects.

These operations construct no dialogs. The conditions that raise a
`QMessageBox::warning` today — a zero fine/coarse increment, a division by
zero in `set_value` — become distinct `Error`s that the UI maps to the same
modal it shows now. Nothing here needs `IEventSink`: the callers are already in
the UI layer, where `QMessageBox` is legitimate.

Two supporting free functions carry the byte codec, extracted from
`get_rom_data_value` / `set_rom_data_value`:

```cpp
Result<std::int64_t> read_raw_element(bytes::ByteView rom_data, const MapElementSpec& spec,
                                      std::uint32_t index);
Result<std::vector<std::uint8_t>> encode_scaled_value(const MapElementSpec& spec, double display_value,
                                                      int float_precision);
```

`encode_scaled_value` is the inverse of the per-element step inside
`decode_scaled_values` (`calibration_service.h:101-122`), and is tested against
it by round trip.

### Two consequences of the move

**The `union map_data` type-punning goes.** Six copies of

```cpp
union map_data { int8_t sbyte_value[4]; … float float_value; } map_data_value{};
```

write one member and read another — UB in C++, and in
`inc_dec_value:306-310` the read of `float_value` happens after only
`dword_value` was written. The extracted code uses `std::bit_cast`.

**The action string stops being re-parsed.** `menu_action_triggered` already
decodes the `QString` to a typed `MenuCommand` at line 9, then passes the raw
string down so `inc_dec_value` and `interpolate_value` can re-compare it
against `"coarse_inc"`, `"interpolate_horizontal"`, and so on. Passing the
typed value down deletes those comparisons — and with them the single call site
of `menu_command_from_id(const QString&)`, which is the whole of
`//src/algorithms/menu:qt_compat`.

That settles a contradiction between two live designs: the flash drain's
completion criteria claim it deletes `//src/algorithms/menu:qt_compat`, while
the 6a design assigns it to step 6. The 6a design is right — the shim's only
consumer is `src/ui/desktop/menu_actions.cpp:3`, which the drain does not own,
so the drain cannot delete it. **This step deletes it, and the drain design's
criterion is amended to drop that line.**

## Defects found in the write path

Reading the read and write paths side by side turned up five defects — (a)
through (e) below. None is a refactoring artifact; all five are in `master` at
`6dc8c5f7`.

Four more were found later, while implementing PR #274, and are recorded here
alongside the original five rather than in a separate list: (f), (g), and (h)
are write-path defects like the first five, and the fourth is deliberately
left unlettered because it is a grid-repaint defect with no ROM-byte
consequence. All four were found and fixed inside PR #274 itself, which is why
none of them has a pinning test or appears in the sequencing plan below.

The project's convention is to preserve legacy behavior verbatim and document it
(`decode_scaled_values`' own comments say "this is not a bug to fix"). These are
treated differently because they are write-path defects on a tool whose output
gets flashed to an ECU. **The policy for this step is characterize, then fix
separately:** the extraction PRs preserve behavior bit-exact and land a test
pinning each defect, named for it; 6b-4 fixes each and flips its test.

Defects (d) and (e) are the exceptions. Neither can be characterized by a test
that is expected to pass, and neither survives the extraction as a mechanical
matter — an unbounded modal loop and a stack buffer overflow cannot be
reproduced inside a function that returns a patch and sizes its buffer from its
input. Both are fixed in 6b-3 rather than pinned, with a test asserting the
fixed behavior. That leaves **(a), (b), and (c) for 6b-4.**

### (a) The `wrx02` address fixup differs between read and write

**Status: still open, deliberately deferred (not this step).** The plan
required confirming which predicate is correct "against a real `wrx02`
definition" before landing a fix, and named the fallback explicitly: "if the
correspondence cannot be confirmed... stop and report... an unproven fix
here is deferred with the finding recorded, not guessed." Measured at
closeout: `grep -rl wrx02` across both the `mmc-definitions` and
`mmc-patches` corpora finds **zero** ROMs anywhere that declare `wrx02` as
their flash method, so there is no real definition to confirm against.
`element_byte_address` still keeps its `for_write` parameter and both
divergent predicates unchanged, and
`PinnedDefect_Wrx02FixupDiffersBetweenReadAndWrite`
(`map_edit_test.cpp`) is still pinned, unflipped. Recorded as a deferred
action in the [tech-debt roadmap](../../tech-debt.md) under "P1: Separate UI
from application logic."

```cpp
// get_rom_data_value:1646
if (ecuCalDef[rom_number]->RomInfo.at(FlashMethod) == "wrx02" &&
    ecuCalDef[rom_number]->FileSize.toUInt() < byte_address)
// set_rom_data_value:1735
if (ecuCalDef[rom_number]->RomInfo.at(FlashMethod) == "wrx02" &&
    ecuCalDef[rom_number]->FileSize.toUInt() < (190 * 1024) && byte_address > 0x27FFF)
```

Two different predicates over the same ROM, both subtracting `0x8000`. A 180 KB
`wrx02` image with a cell at `0x28000`: the read predicate is false
(`184320 < 163840` is false) so the value is read from `0x28000`; the write
predicate is true (`184320 < 194560` and `163840 > 0x27FFF`) so the edit is
written to `0x20000`. The displayed byte and the edited byte are different
bytes.

The write-side predicate is the one that matches the documented
`apply_flash_method_padding` special case
(`calibration_service.h:68-80`: inserts `0x8000` bytes at offset `0x20000` when
the image is under `190 * 1024`), which suggests the read side is the wrong
copy. **The fix must be justified against the `wrx02` family rather than
chosen on this reasoning alone** — see [6b-4](#sequencing).

### (b) The edit path ignores the strided layout the read path honours

**Status: fixed (commit `2468fcc0`).** `MapElementSpec` gained
`start_position`/`interval` fields (default 1, matching "no striding"), and
`element_byte_address` now applies `decode_scaled_values`' exact
`address + (start_position - 1) * width + index * width * interval` formula
on both the read and write side. Corpus measurement taken for the fix (not
re-run at closeout): `grep -rhoE '(startpos|interval)="[^"]*"'` over the 59
EcuFlash XML definitions in `mmc-definitions/site/xml/` returns no matches at
all — 0 of 59 files carry a non-default `startpos` or `interval`. Per the
plan's zero-count fallback, the fix is unproven against real striding data
but lands anyway because the divergence from `decode_scaled_values` was a
latent trap regardless of whether any current definition exercises it. The
`HonoursTheStartPositionAndIntervalStride` and the strided case in
`EncodeScaledValue.RoundTripsThroughReadRawElementForEveryWidth`
(`map_edit_test.cpp`) cover it.

`decode_scaled_values` lays elements out as

```text
addr(j) = address + (start_position - 1) * width + j * width * interval
```

`get_rom_data_value` and `set_rom_data_value` use a flat
`address + index * width`.

`startpos` and `interval` are real EcuFlash table attributes: parsed in
`parser_utils.cpp:331-336` and `:402-407`, defaulted to 1 in
`definition_resolver.cpp:350-351` and `:377-378`, and carried into the Qt model
as `StartPosList` / `IntervalList` (`legacy_definition_adapter.cpp:225-226`).
The edit path reads neither list. So for any map with `interval != 1` or
`start_position != 1`, the grid displays strided values and an edit writes
contiguous ones, landing on neighbouring data.

This repository holds no ROM-definition corpus, so the population of affected
maps is unmeasured here. Measuring it is the first task of 6b-4.

### (c) Bounds enforcement differs per operation

**Status: fixed (commits `7b3cd134..93743ce9`, corrected in the PR 6b-4
final-review fix wave).** Three of the four operations —
`apply_set_expression`, `apply_interpolation`, and `apply_paste` — now encode
through a shared `encode_guarded` helper sitting in front of the byte packing:
it clamps to `MinValueList`/`MaxValueList` and applies the storage-type
**range check**, and a firing is a **whole-operation failure**
(`ErrorKind::InvalidConfig`) — nothing in the selection is written.
`apply_paste`, previously bounds-free entirely, is one of those three.

`apply_increment` does **not** call `encode_guarded`. It shares only the
underlying guard logic, through its own per-cell bounded retry loop, which
reverts just the one cell whose candidate fired the guard and continues with
the next cell rather than failing the whole edit. Its clamp-then-retry control
flow cannot be expressed through `encode_guarded`'s `Result`-returning shape,
and its tested behavior is unchanged by this section's fixes.

**The two paths deliberately apply different amounts of the legacy guard, and
that split is the point.** Legacy's guard block ORs two unrelated
sub-conditions together per signed storage type: a genuine range check (the
candidate's byte pattern would represent a value the width cannot hold, and
the pre-edit value did fit) and a *sign-wrap heuristic* (the byte's high bit
was set before the edit and is clear after). The heuristic is legacy's crude
proxy for "this increment wrapped around from positive overflow into a
negative bit pattern" — but it fires on **any** negative-to-positive
transition of a signed cell, in range or not, so `int8` `-5 → 3` trips it.
That is only defensible where a false positive costs one cell, which is
exactly `apply_increment`'s revert-and-continue loop. Applied as a hard
failure it rejects ordinary valid edits on any signed-storage map — timing
trim, fuel trim, MAF correction are all routinely `int8`/`int16`. The first
6b-4 pass had `encode_guarded` apply the whole legacy guard including the
heuristic; the final-review fix wave (finding C1) narrowed it to the range
check alone, split out as `storage_range_check_fires` in `map_edit.cpp`
alongside `sign_wrap_heuristic_fires`, which only `apply_increment`'s
`apply_saturation_guard` reaches. The regression is covered by an
in-range-negative-to-positive test on each of the three `encode_guarded`
callers plus a genuinely-out-of-range counterpart, and by
`ApplyIncrement.SignWrapHeuristicStillRevertsAnInRangeNegativeToPositiveIncrement`
pinning that `apply_increment` still applies both halves. Every pre-existing
guard test used `uint8` storage, where the heuristic and the range check
coincide, which is why none of them caught it.

A first pass (`61b4de54`) routed `apply_set_expression` and
`apply_interpolation` through `encode_guarded` but let it also override
their formatting precision with `float_precision`, contradicting both
functions' own documented precision-6 contract; a follow-up (`93743ce9`)
gave `encode_guarded` a separate `format_precision` parameter, restored
precision-6 for `apply_set_expression`/`apply_interpolation`, and added
regression tests for the precision divergence plus clamp/guard coverage on
`apply_interpolation` and `apply_paste`'s guard-rejection path.

- `inc_dec_value` clamps to `MinValueList`/`MaxValueList` **and** runs
  storage-type saturation and sign-wrap guards (`:347-400`), reverting the cell
  to its previous raw value when an encode would overflow `uint8`/`int8`/
  `uint16`/`int16`/`uint32`/`int32`. The block's own text also names `float`,
  but that arm is dead: the `float` comparison sits inside a
  `storagetype.startsWith("int")` branch that `"float"` can never satisfy, so
  float-storage maps get no saturation guard at all. `int24`/`uint24` have no
  width clause either. Both quirks are carried into the port verbatim rather
  than fixed — they change which values are *rejected*, not which bytes a
  write produces, and reconciling them is not part of this defect.
- `set_value` clamps min/max (`:594-601`) and has none of the saturation
  guards.
- `paste_value` has neither. Clipboard text goes through `to_byte` straight
  into ROM bytes (`:985-1001`) with no bounds check at all.

`paste_value` is the hardware-facing one: arbitrary clipboard content becomes
arbitrary ROM bytes.

### (d) The zero-increment warning is inside the retry loop

In `inc_dec_value`, the guard

```cpp
if (map_coarse_inc_value == 0 || map_fine_inc_value == 0)
    QMessageBox::warning(this, tr("Set value"),
                         "Fine / Coarse inc value not set or set to zero in definition file!");
```

sits at `:276-281`, inside `do { … } while (rom_data_value == new_rom_data_value);`
(`:274-401`). The loop exists to retry until the raw value actually changes.
With a zero increment `map_item_value` never changes, so the encoded value keeps
matching and the loop re-runs, raising the modal again — per selected cell,
unboundedly. Whether it terminates depends on whether the `to_byte`/`from_byte`
round trip happens to perturb the value, which is not a termination argument.

Note that this modal is a `QMessageBox` in the UI layer, so it is not a
`//:backend_no_widgets` violation and does not need to become an `IEventSink`
notice. It stays a `QMessageBox`; what moves into `map_edit` is the *decision*
to warn, as an error kind returned once instead of a modal raised per
iteration.

**Fixed by the extraction in 6b-3, not pinned.** Returning that decision as an
error is what the [whole-operation failure rule](#the-edit-operations)
requires, and once the warning is a returned error the retry loop has nothing
left to retry: a zero increment fails the call. There is no way to move this
function into a patch-returning operation and still reproduce the loop, so
there is no version of this defect to characterize.

### (e) `interpolate_value` overflows a fixed stack array

`interpolate_value:753` declares

```cpp
float cellValue[128][128];
```

— 64 KB of stack — and indexes it as `cellValue[i][j]` with `i` running to
`interpolateColCount - 1` and `j` to `interpolateRowCount - 1`, both taken
directly from the selected range. A selection wider or taller than 128 writes
past the array. The commented-out line above it
(`// float cellValue[interpolateColCount][interpolateRowCount];`) shows the
author reaching for a VLA and settling for a fixed bound.

**Fixed by the extraction in 6b-3, not pinned**, for the same reason as (d): a
stack buffer overflow cannot be characterized by a test that is expected to
pass. The portable operation sizes its buffer from the selection. 6b-3 lands a
test with a 200-column selection that would have overflowed.

### (f) `read_raw_element`'s signed multi-byte reads were byte-swapped, and int24 always read as zero

Found during, and fixed by, PR #274 ("fix the unambiguous write-path
defects" — already merged, predates the defects (a)-(e) analysis above,
which was written from the state PR #274 landed).

The unsigned branch of `get_rom_data_value` assembles a value from
`data_byte` — an array already reordered into the correct endian sequence
before assembly. The signed branch instead reconstructed the value
MSB-first regardless of the spec's endian, so a signed multi-byte read that
should sign-extend the correctly-assembled unsigned value instead byte-swapped
it: the raw bits round-tripped through `encode_scaled_value` came back wrong
for every signed width at both endians. A 24-bit signed read fared worse — it
always returned zero, because the signed-reconstruction branch's `int24`
case never populated a sign bit from a byte the unsigned path had already
assembled correctly.

**Fix:** the signed branch now reuses the same endian-correct `data_byte`
assembly the unsigned branch already had, and applies `sign_extend(data_byte,
width)` — a one-line fix that resolves both defects at once, since they
shared the same root cause (the signed branch's own byte reconstruction
instead of reusing the unsigned path's). Verified against
`decode_scaled_values`' identical assembly-then-sign-extend pattern.
`EncodeScaledValue.RoundTripsThroughReadRawElementForEveryWidth`
(`map_edit_test.cpp`) now exercises every signed width at both endians, plus
`Int24`, "previously untestable here because it always read back as 0." A
now-deleted pinning test,
`PinnedDefect_SignedMultiByteDoesNotRoundTripBecauseTheReadIsByteSwapped`,
characterized the pre-fix behavior; its premise is what the fix falsifies.

### (g) The write path's byte order was inverted relative to its endian label

Found during, and fixed by, PR #274, alongside (f).

`set_rom_data_value` wrote raw values in the *opposite* byte order from what
the element's `endian` field claimed and from what `decode_scaled_values`
(the load path) actually used to interpret bytes on read — a raw `0x1234`
labeled `"big"` was written as `[0x34, 0x12]`. Every non-byte-width write was
affected: the value displayed after a re-read of the map did not match the
value the user entered, because the read path decoded the (wrongly-ordered)
bytes back with the opposite convention and happened to land on a different
number.

**Fix:** `encode_scaled_value` now always writes the byte order the spec's
`endian` label claims, matching `decode_scaled_values` and
`read_raw_element`'s own (post-(f)) endian handling exactly. Float storage is
unaffected by `endian` in either direction — it is always written
big-endian-in-ROM regardless of the label, matching the load path's existing
convention. `EncodeScaledValue.EncodesInTheLabeledByteOrder`
(`map_edit_test.cpp`) pins the corrected behavior; the now-deleted
`PinnedDefect_WriteOrderDivergesFromLegacyBecauseLegacyIsAlsoByteSwapped` and
`LegacyByteOrderFlagSelectsWriteOrder` characterized the pre-fix inversion
and the `legacy_byte_order` flag that used to select it (also removed).

### (h) Float-storage calibration writes stored garbage

Found during, and fixed by, PR #274, alongside (f) and (g). A distinct root
cause from both — not a byte-order bug.

The write path parsed the cell's edited decimal text with `QString::toInt()`
and packed the resulting *integer* as if its bit pattern were the float's raw
storage bytes, rather than converting the actual entered value into the
float's IEEE-754 bit pattern. Editing a float-storage cell to, for example,
`1.5` wrote whatever bytes `QString::toInt()` happened to produce from
parsing `"1.5"` (an integer parse of a decimal string), not `1.5`'s actual
float bit pattern — silently corrupting every float-storage calibration edit.

**Fix:** `raw_element_value_from_text` (`src/ui/desktop/calibration/
map_edit_adapter.{h,cpp}`), a new UI-adapter function, converts the edited
text to the element's actual value and `bit_cast`s it into the raw storage
representation, paired with its already-tested inverse,
`format_raw_element_value`, which formats a raw value back to display text.
Both live in the UI adapter package (not `map_edit` itself) because the
conversion needs `QString` at the text-entry boundary; `map_edit`'s own
`encode_scaled_value` deals only in already-converted `double`/raw values and
was not itself the source of this defect.

### A fourth PR #274 fix, deliberately unlettered

PR #274 also fixed `map_cell_color_scale`'s equal-min/max-bounds
division-by-zero (previously producing a non-finite hue) to a defined `0.0`.
It is not given a letter here: this section is scoped to write-path
defects — "a tool whose output gets flashed to an ECU" — and the color scale
is a grid-repaint helper with no ROM-byte consequence at all. Recorded here
only so the record of what PR #274 fixed is complete.

## Testing

Every new test is `fastecu_portable_gtest` — no Qt, no `QApplication`, no
offscreen platform. That is the point of the slice: today not one line of this
code is reachable without a live `QMainWindow`, `QMdiSubWindow`, and
`QTableWidget`, which is why none of it has ever had a test.

- **Byte codec** (`map_edit_test.cpp`): each storage type × endian, the
  `wrx02` fixup on both sides, out-of-range windows reported as
  `ErrorKind::Internal` rather than read out of bounds (matching
  `decode_scaled_values`' choice), and a round-trip property —
  `read_raw_element` after applying `encode_scaled_value`'s bytes returns the
  input raw value — asserted against `decode_scaled_values` so the two sides
  are pinned to each other.
- **Target resolution:** map body, X axis, Y axis; both static scale types
  rejected; the 1×N and N×1 degenerate cases; and the offset adjustments each
  branch makes, including the Y-axis `x_size = 1` override.
- **Edit operations:** increment, set-expression (`+`, `-`, `*`, `/`, bare
  value, and division by zero), the three interpolation modes, and paste — each
  over a synthetic ROM, asserting both the returned bytes and the display text.
- **Pinning tests** for the defects 6b-4 was to fix, named for the defect and
  carrying a comment pointing at this document's section. The plan called for
  three, one each for (a), (b), and (c); what shipped differs, and the current
  state is:
  - **(a)** — `PinnedDefect_Wrx02FixupDiffersBetweenReadAndWrite` is the one
    pinning test still pinned, deliberately, because (a) itself is deferred
    (see [its status note](#a-the-wrx02-address-fixup-differs-between-read-and-write)).
  - **(b)** — never had a pinning test. The flat-layout behavior could not be
    pinned as a passing assertion about an address without asserting the wrong
    address as correct, so Task 14 added a new positive test
    (`HonoursTheStartPositionAndIntervalStride`, plus a strided case in the
    round trip) asserting the fixed layout instead of flipping an existing one.
  - **(c)** — had two pinning tests, on `apply_set_expression` and
    `apply_paste`; Task 13 flipped both into
    `RejectsAValueThatWouldOverflowTheStorageType` and
    `ClampsAPastedValueToTheDefinitionMaximum`. The final-review fix wave added
    the signed-storage cases the `uint8`-only suite could not distinguish (see
    [(c)](#c-bounds-enforcement-differs-per-operation)).

  Defects (d) and (e) instead get ordinary tests asserting the fixed behavior —
  a zero increment returns an error rather than looping, and a 200-column
  selection interpolates without overflowing. Defects (f), (g), and (h) were
  found and fixed within PR #274, so nothing about them was left pinned: the
  pinning tests 6b-1 had landed for the byte-order behavior were deleted by
  #274 when it fixed the underlying defects, and are named in
  [(f)](#f-read_raw_elements-signed-multi-byte-reads-were-byte-swapped-and-int24-always-read-as-zero)
  and [(g)](#g-the-write-paths-byte-order-was-inverted-relative-to-its-endian-label).

The warning conditions are asserted as returned `Error`s, not through
`RecordingEventSink` — see [the edit operations](#the-edit-operations) for why
this slice needs no event sink.

No bench re-qualification, and no bench-notes or qualification-matrix entry.
This slice changes no wire behavior, and the
[flash qualification matrix](../../flash-qualification-matrix.md) is untouched
by every PR in the step — deliberately, not by omission. That matrix has one
row per *flash operation family*: a class reached through
`protocols.cfg`, keyed by transport (K-Line / raw CAN / ISO-15765 / JTAG /
BDM) and scoped to `read`/`test_write`/`write` against a live ECU. Editing a
calibration map is none of those things — it mutates a ROM image in memory and
on disk, before any family is selected — so there is no row it belongs to and
no row whose `hardware_evidence` this step invalidates. Whichever family
eventually flashes the edited image is unaffected by which bytes the image
contains; every one of those rows is already `experimental` or `unqualified`
on its own terms.

What *is* true, and is recorded here rather than in that matrix: steps 6b-1
through 6b-4 changed which bytes a calibration edit produces. PR #274 fixed
signed multi-byte reads, int24 reads, the write path's inverted byte order,
and float-storage writes (defects [(f)](#f-read_raw_elements-signed-multi-byte-reads-were-byte-swapped-and-int24-always-read-as-zero),
[(g)](#g-the-write-paths-byte-order-was-inverted-relative-to-its-endian-label),
and [(h)](#h-float-storage-calibration-writes-stored-garbage)); 6b-4 changed
the layout an edit targets on a strided map
([(b)](#b-the-edit-path-ignores-the-strided-layout-the-read-path-honours)) and
which edits are rejected outright
([(c)](#c-bounds-enforcement-differs-per-operation)). The operator-facing
consequence is about *existing files*, not about the flashing path: a
calibration edited with a pre-#274 build of this tool on a signed multi-byte,
int24, or float-storage map may hold bytes that do not match what its grid
displayed, and re-opening it in a current build will now decode those bytes
correctly rather than reproducing the old mistake. Such a file should be
re-checked against its definition before it is flashed. That is a data
provenance caution, not a hardware-qualification gate, which is why it is
stated here and not entered as matrix evidence.

## Sequencing

Four PRs, each independently mergeable and each green on
`bazel test --config=release //...` before the next starts.

1. **6b-1 — byte codec.** `get_rom_data_value` / `set_rom_data_value` become
   `read_raw_element` / `encode_scaled_value` in the new `map_edit` target,
   verbatim, with both `wrx02` predicates preserved as two now-visibly-different
   parameters in one file. `union` punning becomes `std::bit_cast`. Deletes
   `check_rom_data_value`. Registers `map_edit` in the `genquery` in
   `BUILD.bazel` and in `PORTABLE_ROOTS`. About 190 lines leave
   `menu_actions.cpp`.
2. **6b-2 — target resolution and display helpers.** `resolve_edit_target` and
   `MapElementSpec`; the three duplicated axis-resolution blocks collapse into
   one UI adapter that calls it. `get_mapvalue_decimal_count` and the
   `get_map_cell_colors` HSV scale computation go portable; the `QColor`
   conversion stays in the UI. About 150 lines net.
3. **6b-3 — edit operations and shim drain.** The four `apply_*` functions; the
   four UI slots become resolve → collect input → call → apply patch → repaint,
   taking a typed `MenuCommand`. Fixes defects (d) and (e) by construction.
   Deletes `//src/algorithms/menu:qt_compat`, `qt_menu_command.h`, and
   `menu_command_qt_compat_test.cpp`.
4. **6b-4 — the three remaining fixes**, one commit each, each flipping its
   pinning test.
   Defects (a) and (b) change which ROM byte a write targets, so each needs
   evidence before a fix is chosen: for (b), a count of maps with
   `interval != 1` or `start_position != 1` across the EcuFlash definitions in
   `mmc-definitions/site/xml/` and the RomRaider corpus; for (a), confirmation
   against a `wrx02` image of which predicate matches the padding rule. **If
   the evidence for either is inconclusive, that fix is deferred with the
   finding written into the tech-debt roadmap rather than guessed.** Defect (c)
   needs no corpus evidence and lands regardless.

   **Outcome:** (c) landed first (commits `7b3cd134..93743ce9`), needing no
   corpus evidence as predicted. (b)'s corpus count came back zero rather
   than inconclusive-but-nonzero — no definition in `mmc-definitions/site/xml/`
   carries a non-default `startpos`/`interval` at all — and per the plan's own
   zero-count fallback the fix landed anyway (commit `2468fcc0`) because the
   divergence was a latent trap independent of whether current data exercises
   it. (a)'s evidence search came back with no `wrx02` ROM to confirm
   against in either the `mmc-definitions` or `mmc-patches` corpus, which
   *is* the inconclusive case this paragraph anticipated, so (a) is deferred
   exactly as specified — see its
   [status note](#a-the-wrx02-address-fixup-differs-between-read-and-write)
   and the [tech-debt roadmap](../../tech-debt.md).

## Completion criteria

Exact and machine-checked.

- `grep -nE 'union map_data|FullRomData\[' src/ui/desktop/menu_actions.cpp`
  matches nothing.
- `//src/algorithms/menu:qt_compat` does not exist, and neither do
  `src/algorithms/menu/qt_menu_command.h` nor
  `src/algorithms/menu/menu_command_qt_compat_test.cpp`.
- `//src/backend/calibration:map_edit` appears both in the `genquery` in
  `BUILD.bazel` and in `PORTABLE_ROOTS` in
  `scripts/check-portable-closure.py`, and `//:portable_closure` passes.
- `check_rom_data_value` appears nowhere in the tree.
- `src/ui/desktop/menu_actions.cpp` is under 1400 lines, from 2138.
- The branch's `git diff --name-only origin/master` contains no path under
  `src/platform/desktop/common/flash/legacy/` or `src/ui/desktop/flash/`.

## Risks

- **6b-1 and 6b-3 change ROM-mutation code paths.** The round-trip test against
  `decode_scaled_values` is the safety net: a divergence surfaces as a failed
  round trip rather than as a silently wrong calibration.
- **`resolve_edit_target`'s rule is inferred from three copies that differ.**
  The design takes `inc_dec_value` as canonical because it is the only copy that
  reads every field. If 6b-2 finds a fourth behavioral difference between the
  copies, the extra behavior is pinned as its own test and adjudicated in this
  document before the collapse lands.
- **The `map_edit` target is registered in two places.** Forgetting the
  `genquery` half means the closure check passes vacuously — the failure mode
  6a-5 hardened `//:backend_no_widgets` against. 6b-1 proves the registration
  non-vacuous the same way step 4 did: inject a `QT_DEPS` dependency into
  `map_edit`, observe `//:portable_closure` fail, restore.
- **This step does not shrink the `serial_qt_compat` allowlist.**
  `//src/ui/desktop:__pkg__` stays on it; `menu_actions.cpp:5` includes
  `serial_port_actions.h` for `connect_to_ecu` and the OBD/CAN utilities, none
  of which this slice touches. That entry is later step-6 work.
