#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Which of a map's three element runs an edit targets. Declared here rather
// than beside resolve_edit_target (Task 5) because the UI adapter (Task 4)
// needs it to choose which parallel-list group to pluck from, and that lands
// first.
enum class EditTargetKind
{
    MapBody,
    XAxis,
    YAxis,
    Rejected,
};

// A selection rectangle in the map grid widget's own coordinates: row 0 and
// column 0 are the axis header row/column, data cells start at (1, 1). This
// is what resolve_edit_target consumes -- QTableWidgetSelectionRange's
// leftColumn()/topRow()/rightColumn()/bottomRow(), carried as plain data so
// the rule is testable without a QTableWidget.
struct SelectionRange
{
    int first_row{0};
    int first_col{0};
    int last_row{0};
    int last_col{0};
};

// A map's element-grid extent, as read from XSizeList/YSizeList.
struct MapDimensions
{
    std::uint32_t x_size{0};
    std::uint32_t y_size{0};
};

// The result of resolving a widget selection to the element run it targets:
// which run (`kind`), the selection translated into element coordinates
// (`range`), and the element width of that run (`x_size` -- 1 for YAxis,
// unchanged for XAxis/MapBody, since legacy overrides mapXSize to 1 only on
// the Y-axis branch).
struct EditTarget
{
    EditTargetKind kind{EditTargetKind::MapBody};
    SelectionRange range;
    std::uint32_t x_size{0};
};

// Decides which of a map's three element runs (body / X axis / Y axis) a
// widget-coordinate selection targets, reproducing the three-way branch
// duplicated verbatim across inc_dec_value, set_value, and interpolate_value
// in menu_actions.cpp -- inc_dec_value is the canonical copy transcribed
// here, being the only one that reads every field this rule governs.
//
// `selection` arrives in widget coordinates (see SelectionRange); the
// returned `range` is in element coordinates, with legacy's `-1` applied to
// every bound first, then each branch's own `++` adjustments folded in:
//   - leftColumn() == 0 with a multi-row map targets the Y axis: both column
//     bounds shift back by the same `-1`/`+1` pair, and x_size collapses to
//     1 (the Y axis is one element wide regardless of the map's own
//     x_size).
//   - otherwise, topRow() == 0 with a multi-column map targets the X axis:
//     both row bounds get the same shift-back.
//   - otherwise it's the map body: a 1-column map (x_size == 1) still shifts
//     its row bounds back (there is no row-0 header to reserve), and a
//     1-row map (y_size == 1) shifts its column bounds back, symmetrically.
// A "Static X Axis"/"Static Y Axis" scale type on the X-axis or Y-axis
// branch rejects the edit outright (legacy's early `return` from the
// enclosing UI handler), since static axes aren't editable.
EditTarget resolve_edit_target(const SelectionRange& selection, MapDimensions dims, std::string_view x_scale_type);

// One run of editable elements: a map's cells, or one axis's points. The
// non-owning counterpart of calibration_service.h's ElementRun, for the write
// side. string_view fields borrow from the EcuCalDefStructure lists the UI
// adapter reads them out of, which always outlive the call. Never store one.
struct MapElementSpec
{
    std::uint64_t address{0};
    std::optional<definition::StorageType> storage_type;
    std::string_view endian;
    std::string_view to_byte{"x"};
    std::string_view from_byte{"x"};
    // " " means "unset" in the legacy model, not "empty". Compared as text
    // before conversion, exactly as the legacy code does.
    std::string_view min_value{" "};
    std::string_view max_value{" "};
    double coarse_increment{0.0};
    double fine_increment{0.0};
    // The map's own geometry (from XSizeList/YSizeList), NOT the resolved
    // edit-run width. In particular this does NOT carry the Y-axis override
    // to 1 that EditTarget::x_size (above) does -- indexing a Y-axis edit's
    // cells with this field instead of the resolved EditTarget::x_size
    // silently writes to the wrong ROM offset. Nothing in this file
    // currently reads these fields for indexing; any future caller doing
    // per-cell indexing math from a MapElementSpec must use the resolved
    // width, not this one.
    std::uint32_t x_size{1};
    std::uint32_t y_size{1};
    // The strided-layout inputs (spec's defect (b)): decode_scaled_values
    // lays elements out at address + (start_position-1)*width +
    // index*width*interval, not a flat address + index*width. Defaulted to 1
    // (no striding, matching decode_scaled_values' own EcuCalDefStructure
    // default) so a spec built without setting these behaves exactly as the
    // old flat layout did.
    std::uint32_t start_position{1};
    std::uint32_t interval{1};
    // The wrx02 relocation inputs. Carried as data rather than read from a
    // global so the rule is testable; see the spec's defect (a).
    std::string_view flash_method;
    std::uint64_t rom_file_size{0};
};

// Byte address of element `index`, including the wrx02 relocation. Exposed
// because the read and write paths apply *different* wrx02 predicates today
// and the difference must be visible and separately testable -- see the
// spec's defect (a). `for_write` selects set_rom_data_value's predicate;
// otherwise get_rom_data_value's is used.
std::uint64_t element_byte_address(const MapElementSpec& spec, std::uint32_t index, bool for_write);

// The raw stored value of element `index`, reproducing get_rom_data_value.
//
// Returns ErrorKind::Internal when the element's window would run past
// rom_data's end; legacy indexed QByteArray::at() unchecked here.
Result<std::int64_t> read_raw_element(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t index);

// The byte-packing half of encode_scaled_value: writes an ALREADY-ENCODED
// raw value, with no expression evaluation and no rounding. This is the
// faithful portable equivalent of legacy set_rom_data_value, whose callers
// hand it a raw value smuggled through a float bit pattern (see
// menu_actions.cpp's `map_data_value.dword_value = new_rom_data_value.toInt();`
// followed by passing `map_data_value.float_value` through the float
// parameter, which round-trips the same bits unchanged) -- set_rom_data_value
// itself performs no expression evaluation or rounding, only byte packing.
//
// For `StorageType::Float`, `raw`'s low 32 bits ARE the float's bit pattern
// (not a numeric value to convert): legacy's `map_data_value.float_value`
// argument already carries the encoded bits, and set_rom_data_value packs
// `.byte_value[]` -- an alias over the same union storage -- directly,
// without ever re-interpreting them as a floating-point number. Packed here
// the same way: bit-for-bit, via the low 32 bits of `raw`.
//
// Writes the byte order `spec.endian`'s label claims (matching
// decode_scaled_values, the correct decoder, and read_raw_element's own
// endian handling) for every non-float width; float storage is always
// written big-endian-in-ROM regardless of `spec.endian`, matching
// decode_scaled_values and read_raw_element's float handling.
Result<std::vector<std::uint8_t>> write_raw_element(const MapElementSpec& spec, std::int64_t raw);

// Encodes `display_value` into `spec`'s on-ROM byte representation. Runs
// `display_value` through spec.to_byte via expression_evaluate, using the
// same format_like_qt_g formatter decode_scaled_values uses, so the two
// cannot drift on how a double becomes the string an expression sees, then
// rounds (llround for non-float, bit_cast for float) and delegates the
// actual byte packing to write_raw_element -- the two functions cannot drift
// on byte order because there is only one packing loop.
//
// This is the "display value in, on-ROM bytes out" half of a calibration
// edit -- the primitive the edit operations in Tasks 8-11 need. It is NOT a
// faithful port of legacy set_rom_data_value: that function is a raw byte
// writer with no expression evaluation of its own (see write_raw_element).
// menu_actions.cpp's edit loops must therefore call write_raw_element with
// the raw value they already compute via their own to_byte/round arithmetic,
// not this function -- routing set_rom_data_value's callers through
// encode_scaled_value would re-run to_byte (and round with a different
// primitive: llround/double here vs. legacy's qRound/float), which is only
// a no-op when to_byte/from_byte are exact inverses and format_like_qt_g is
// lossless at the configured precision. Neither is guaranteed for real
// definitions.
//
// Writes the byte order spec.endian's label claims (matching
// decode_scaled_values, the correct decoder, and read_raw_element's own
// endian handling) for every non-float width; float storage is always
// written big-endian-in-ROM regardless of spec.endian, matching
// decode_scaled_values and read_raw_element's float handling. (Legacy
// set_rom_data_value's write order was inverted relative to its own endian
// label -- raw 0x1234 labeled "big" wrote [0x34, 0x12] -- a defect fixed
// alongside read_raw_element's signed-multi-byte byte-swap, since the two
// were the same underlying bug on opposite sides of the read/write pair.)
Result<std::vector<std::uint8_t>> encode_scaled_value(const MapElementSpec& spec, double display_value,
                                                      int float_precision);

// The single guarded encode path every map edit operation now goes through
// -- the spec's defect (c) fix. Before 6b-4, the four apply_* operations each
// did a different ad-hoc subset of bounds enforcement: apply_increment
// clamped to spec.min_value/max_value AND ran a storage-type
// saturation/sign-wrap guard; apply_set_expression clamped but never guarded;
// apply_interpolation and apply_paste did neither, so arbitrary interpolated
// or pasted values became arbitrary ROM bytes. encode_guarded unifies this:
// it clamps `display_value` to spec.min_value/max_value (the " " = unset
// convention, same as every clamp elsewhere in this file), encodes the
// clamped value through spec.to_byte, and applies the same storage-type
// saturation/sign-wrap guard apply_increment's own guard applies -- reusing
// the WIDE (pre-truncation) candidate the guard needs, since the packed
// bytes it returns are already narrowed to the storage width and would have
// silently discarded the very overflow the guard exists to catch.
// `rom_data`/`index` are read via read_raw_element to get the guard's
// "previous value" comparison point, exactly as apply_increment's own guard
// does.
//
// `format_precision` and `float_precision` are DIFFERENT things, despite
// both being encode-time precision knobs -- this split exists because a
// single shared precision here would be an unsound reuse of
// encode_scaled_value's own formatting choice (see that function's doc
// comment for exactly this hazard): `format_precision` controls how
// `clamped` is formatted into the "x" input text for spec.to_byte;
// `float_precision` is only expression_evaluate's third argument
// (intermediate-rounding precision for multi-operator expressions), same
// role it plays everywhere else in this file. apply_set_expression and
// apply_interpolation both have a legacy-fidelity contract predating 6b-4 --
// documented in their own doc comments below -- that their "x" input is
// formatted with a bare QString::number call (precision 6), regardless of
// float_precision; they pass `format_precision = 6` here to preserve that
// contract even though they now share this clamp/guard path. apply_paste has
// no such contract (legacy never formatted its "x" input at all -- see its
// own doc comment) and passes `float_precision` for both.
//
// A guard firing is a hard failure here (ErrorKind::InvalidConfig) -- unlike
// apply_increment, which has a bounded retry loop that reverts a single cell
// to its previous value and keeps going, encode_guarded's callers
// (apply_set_expression, apply_interpolation, apply_paste) have no such retry
// mechanism, so the whole-operation-failure rule applies instead. This
// exactly matches the flipped pinning tests: a value that would overflow the
// storage type now fails the whole call rather than truncating silently.
//
// apply_increment itself does NOT call this function -- its own bounded
// retry loop needs the clamp and guard woven into its per-attempt control
// flow (a guard firing must revert the cell and continue, not fail the whole
// call), which this function's Result-returning shape cannot express. It
// keeps its own clamp and calls the same underlying saturation-guard check
// (lifted to file scope in map_edit.cpp so both share one implementation),
// preserving apply_increment's own tested behavior exactly.
Result<std::vector<std::uint8_t>> encode_guarded(bytes::ByteView rom_data, const MapElementSpec& spec,
                                                 std::uint32_t index, double display_value, int format_precision,
                                                 int float_precision);

// Display helper: pure formatting computation pulled out of
// get_mapvalue_decimal_count (menu_actions.cpp). Doesn't touch ROM data;
// feeds how a value is rendered in the map grid widget. (The color-hue
// arithmetic that used to live alongside this as map_cell_color_scale was
// folded back into MainWindow::get_map_cell_colors -- it's a single tiny
// expression with exactly one caller, tightly coupled to the Qt QColor
// conversion it exists to feed, and didn't earn its keep as a separate
// portable function.)

// How many decimal places to render a cell's value with, reproducing
// get_mapvalue_decimal_count. `value_format` is a RomRaider-style format
// string (e.g. "0.00"); legacy counts the `'0'` characters in the segment
// between the FIRST and SECOND '.' -- QString::split(".").at(1), not
// everything after the first '.' -- so a format string with more than one
// '.' only has its first fractional segment counted. No '.' at all returns
// 0.
int map_value_decimal_count(std::string_view value_format);

// One cell's computed edit result: the flat run index (also the index used to
// look up `cell_text` and to call element_byte_address), the value to show in
// the grid afterwards, the ROM offset to write, and the already-encoded bytes
// to write there.
struct CellPatch
{
    std::uint32_t index{0};
    std::string display_text;
    std::uint64_t byte_address{0};
    std::vector<std::uint8_t> bytes;
};

// The set of per-cell writes one edit operation produces. Applying it is the
// caller's job (Task 12) -- these functions never touch rom_data themselves.
using EditPatch = std::vector<CellPatch>;

// Which increment inc_dec_value's `action` string selected: "coarse_inc" /
// "coarse_dec" / "fine_inc" / "fine_dec" in menu_actions.cpp.
enum class IncrementStep
{
    FineUp,
    FineDown,
    CoarseUp,
    CoarseDown,
};

// Ports the arithmetic half of legacy inc_dec_value (menu_actions.cpp) into a
// pure, patch-returning operation: given the current ROM bytes and every
// selected cell's current display text, computes what each cell's new stored
// bytes and new display text should be, without writing anything itself.
//
// `x_size` is the RESOLVED edit-run width used to index `cell_text` (and,
// combined with `range`, to compute each cell's flat index) -- EditTarget::x_size
// from resolve_edit_target, NOT spec.x_size. spec.x_size/y_size carry the
// map's own geometry (see MapElementSpec's doc comment) and indexing with them
// instead would silently mis-index every Y-axis edit, since the Y axis is
// always one element wide regardless of the map's x_size.
//
// Ported from inc_dec_value with exactly three changes, matching the spec's
// defect (d) and the extraction's own requirements -- everything else,
// including the min/max clamping and the storage-type saturation/sign-wrap
// guards, is preserved exactly as legacy has them (string comparisons against
// "uint8"/"int16"/etc., not enum comparisons). The spec's defect (c) -- the
// other three operations' inconsistent bounds enforcement -- is fixed in
// 6b-4 by extracting the saturation/sign-wrap guard this function applies
// into a shared, file-scope check (map_edit.cpp) that encode_guarded also
// calls; this function's own clamp-then-retry control flow is untouched,
// since it can't be expressed through encode_guarded's Result-returning
// shape (see encode_guarded's own doc comment):
//   1. The zero-increment check moves before the loop and fails the whole
//      call with ErrorKind::InvalidConfig, rather than raising a modal inside
//      a retry loop a zero increment could never exit.
//   2. Legacy's `do { ... } while (rom_data_value == new_rom_data_value)`
//      retry is preserved -- it keeps adding the increment until the
//      ENCODED (to_byte-rounded) value actually changes, which matters
//      whenever to_byte maps several display values onto one stored raw
//      value (any scaling coarser than 1:1, e.g. to_byte = "x/2") -- but is
//      now bounded at kMaxIncrementAttempts (1000) attempts, unlike
//      legacy's genuinely unbounded version. This bound, together with the
//      already-described zero-increment check above, is the actual fix for
//      the spec's defect (d): exceeding the bound without the encoded value
//      ever changing (a pathological to_byte that never responds to its
//      input, e.g. a constant expression) fails the whole call with
//      ErrorKind::InvalidConfig instead of looping forever. A guard-fired
//      revert (the min/max clamp or a storage-type saturation/sign-wrap
//      guard) is always terminal, even on an attempt whose resulting value
//      happens to equal the pre-edit value -- see apply_saturation_guard's
//      own doc comment in map_edit.cpp for why that distinction matters and
//      how it's preserved.
//   3. Cells are collected into an EditPatch instead of written through
//      set_rom_data_value.
//
// Whole-operation failure: per the spec's rule, any per-cell read_raw_element
// or write_raw_element failure fails the entire call immediately with no
// partial patch returned -- unlike legacy's read_rom_data_value wrapper,
// which logged a warning and silently substituted "0" for a read past the ROM
// end, this propagates that failure instead.
//
// Every place legacy formats a value via a BARE QString::number(double) call
// (no explicit precision argument -- Qt's default 'g' format, precision 6)
// uses format_like_qt_g(value, 6) here, literally, regardless of
// `float_precision`: this covers feeding the to_byte/from_byte expressions'
// "x" input and producing the final display text. `float_precision` is used
// only where legacy passes fileActions->float_precision explicitly, as the
// third argument to expression_evaluate -- that controls rounding of
// INTERMEDIATE results during multi-operator expression evaluation, a
// different thing from the input string's own formatting precision.
Result<EditPatch> apply_increment(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t x_size,
                                  std::span<const std::string_view> cell_text, const SelectionRange& range,
                                  IncrementStep step, int float_precision);

// Ports the arithmetic half of legacy set_value (menu_actions.cpp) into a
// pure, patch-returning operation: given every selected cell's current
// display text and a raw user-typed `input` string, computes what each
// cell's new stored bytes and new display text should be, without writing
// anything itself.
//
// `input` is the raw user text from the QInputDialog, with commas already
// replaced by periods by the UI (set_value:472). Its grammar is legacy's: a
// leading '+', '-', '*', or '/' applies that operation to each cell's
// current value using the operand text between the FIRST and SECOND
// occurrence of that operator character (QString::split(op)[1] --
// "+5+3".split("+") is {"", "5", "3"}, so the "3" is silently ignored, the
// same shape of gotcha map_value_decimal_count's '.'-segment parsing has);
// anything else is parsed as an absolute value assigned to every cell.
//
// `x_size` is the RESOLVED edit-run width, exactly as apply_increment's own
// `x_size` parameter documents -- EditTarget::x_size from resolve_edit_target,
// NOT spec.x_size.
//
// Ported from set_value with three changes:
//   1. Legacy's divide-by-zero branch showed a modal and then *continued
//      with the unmodified value*. Per the whole-operation failure rule
//      (apply_increment's doc comment), this becomes an
//      ErrorKind::InvalidConfig failure for the WHOLE call instead, checked
//      once before the loop since the divisor is the same for every cell.
//   2. Cells are collected into an EditPatch instead of written through
//      set_rom_data_value.
// 6b-4 adds a third change, fixing the spec's defect (c): each cell's
// candidate value (post-arithmetic, pre-clamp) is now encoded via the shared
// encode_guarded (map_edit.cpp), which clamps to spec.min_value/max_value --
// as this function always did -- AND applies the same storage-type
// saturation/sign-wrap guard apply_increment runs, which set_value never did
// before this fix. Unlike apply_increment's per-cell retry-and-revert, a
// guard firing here is a hard failure for the WHOLE call
// (ErrorKind::InvalidConfig), per the whole-operation failure rule -- there
// is no retry loop here for a fired guard to revert within. display_text is
// derived by decoding the bytes encode_guarded actually returns and running
// them back through from_byte, rather than from the pre-clamp candidate
// value, so display_text and .bytes can never drift apart.
//
// Every place legacy formats a value via a BARE QString::number(double) call
// (no explicit precision argument) uses format_like_qt_g(value, 6) here,
// literally, regardless of `float_precision` -- same rule as
// apply_increment's doc comment, applied at the equivalent call sites in
// set_value.
Result<EditPatch> apply_set_expression(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t x_size,
                                       std::span<const std::string_view> cell_text, const SelectionRange& range,
                                       std::string_view input, int float_precision);

// Which of interpolate_value's three fill patterns to apply: fill each row
// linearly between its own left/right endpoints, fill each column linearly
// between its own top/bottom endpoints, or (the two-pass combination) first
// interpolate the left and right edges down the rows from the selection's
// four corners, then interpolate each row across between its (now-filled)
// edges.
enum class InterpolationMode
{
    Horizontal,
    Vertical,
    Bidirectional,
};

// Ports the arithmetic half of legacy interpolate_value (menu_actions.cpp)
// into a pure, patch-returning operation: given every selected cell's current
// display text, fills the selection according to `mode` and computes what
// each cell's new stored bytes and new display text should be, without
// writing anything itself.
//
// `x_size` is the RESOLVED edit-run width used to index `cell_text` (reading
// the four corners and writing each cell's final patch entry) -- exactly
// apply_increment's own `x_size` parameter, EditTarget::x_size from
// resolve_edit_target, NOT spec.x_size. This is a DIFFERENT width from the
// selection's own local `col_count`/`row_count` (last_col - first_col + 1,
// last_row - first_row + 1) that size the transient interpolation buffer
// below -- the two must not be conflated.
//
// interpolate_value's four corner values and its interior grid are read
// entirely from `cell_text`, never from `rom_data` -- but as of 6b-4,
// `rom_data` IS read, once per cell, by the shared encode_guarded path below
// (to fetch the guard's "previous value" comparison point), so it is no
// longer unused the way apply_paste's still is.
//
// This is the one place the spec's defect (e) is fixed by construction:
// legacy declares a fixed `float cellValue[128][128]` (64 KB on the stack)
// and indexes it with the raw selection extent, so any selection wider or
// taller than 128 writes past the array. Here the transient buffer is a
// `std::vector<double>` sized exactly to the selection
// (`col_count * row_count`), addressed by a small `at(col, row)` helper that
// preserves legacy's `cellValue[i][j]` == `[col][row]` index order (not
// `[row][col]`) at every site -- `double`, not legacy's `float`, is a
// deliberate, sanctioned precision improvement to the internal arithmetic
// only, independent of the string-formatting precision rule below.
//
// All three modes' formulas, including bidirectional's two-pass structure
// (interpolate the left/right edges down the rows first, then each row
// across), are preserved exactly -- legacy interpolate_value ran no min/max
// clamp and no storage-type saturation/sign-wrap guard at all. 6b-4 adds
// both, fixing the spec's defect (c): each interpolated cell value is now
// encoded via the shared encode_guarded (map_edit.cpp), which clamps to
// spec.min_value/max_value and applies the same saturation/sign-wrap guard
// apply_increment runs. A guard firing is a hard failure for the WHOLE call
// (ErrorKind::InvalidConfig) -- there is no per-cell retry-and-revert here,
// unlike apply_increment. display_text is derived by decoding the bytes
// encode_guarded actually returns and running them back through from_byte,
// so display_text and .bytes can never drift apart.
//
// Every place legacy formats a value via a BARE QString::number(double) call
// (no explicit precision argument) uses format_like_qt_g(value, 6) here,
// literally, regardless of `float_precision` -- same rule as
// apply_increment's doc comment, applied at the equivalent call sites in
// interpolate_value (the to_byte expression's "x" input and the final
// display text). `float_precision` is used only where legacy passes
// fileActions->float_precision explicitly, as expression_evaluate's third
// argument.
Result<EditPatch> apply_interpolation(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t x_size,
                                      std::span<const std::string_view> cell_text, const SelectionRange& range,
                                      InterpolationMode mode, int float_precision);

// Ports the arithmetic half of legacy paste_value (menu_actions.cpp) into a
// pure, patch-returning operation: given a clipboard block already split into
// rows/columns by the UI (`pasted_rows`), computes what each in-bounds pasted
// cell's new stored bytes and new display text should be, without writing
// anything itself.
//
// `x_size`/`y_size` are the RESOLVED edit-run's full extent -- unlike every
// other apply_* operation in this file, paste needs BOTH dimensions (not just
// x_size) because its own bounds check -- `(row + first_row) < y_size &&
// (col + first_col) < x_size` -- silently drops any pasted cell that falls
// outside the map, in either dimension. Same caveat as apply_increment's
// `x_size` doc comment: these are NOT spec.x_size/spec.y_size (the map's own
// geometry, with no axis override -- see MapElementSpec's doc comment), they
// are the resolved EditTarget::x_size/y_size from resolve_edit_target.
//
// `cell_text` is unused for computation, kept only for signature parity with
// the other apply_* operations (see apply_set_expression's doc comment for
// the same reasoning): paste_value's per-cell "current value" read
// (`mapDataCellText.at(index)`) always reads back the SAME index it just
// replaced with the pasted text one line earlier -- so the value it reads is
// always the pasted text itself, never a prior cell value from `cell_text`.
// `rom_data` WAS similarly unused before 6b-4 (paste_value never reads from
// ROM; element_byte_address needs no rom_data of its own either), but is now
// read, once per cell, by the shared encode_guarded path below (to fetch the
// guard's "previous value" comparison point).
//
// One behavioral point, deliberate, not a defect to reconcile: legacy's "x"
// input to expression_evaluate was the pasted text itself, not a
// format_like_qt_g-formatted value -- there was no float-to-string
// formatting step anywhere in legacy paste_value. This is no longer strictly
// true as of 6b-4: bounds-checking a pasted value requires parsing it to a
// number first (to compare against spec.min_value/max_value), and
// encode_guarded's `display_value` parameter is that number, reformatted via
// format_like_qt_g on its way back into expression_evaluate -- a necessary,
// intentional consequence of routing paste through the same guarded path
// every other operation uses. For any pasted text that already round-trips
// cleanly through a decimal parse (every case this file's tests exercise),
// the reformatted text is identical to the original, so this is invisible in
// practice.
//
// The spec's defect (c) -- no min/max clamp and no storage-type
// saturation/sign-wrap guard anywhere in legacy paste_value -- is fixed by
// 6b-4: each pasted cell's parsed value is now encoded via the shared
// encode_guarded (map_edit.cpp), which clamps to spec.min_value/max_value and
// applies the same saturation/sign-wrap guard apply_increment runs. A guard
// firing is a hard failure for the WHOLE call (ErrorKind::InvalidConfig).
// This also retires the "display_text is always the pasted text verbatim"
// behavior described above in earlier revisions of this comment:
// `CellPatch::display_text` is now derived the same way every other apply_*
// operation derives it -- decoding the bytes encode_guarded actually returns
// and running them back through from_byte -- so a clamped value's displayed
// text always matches what was actually written, and display_text/.bytes can
// never drift apart. For pasted text that parses to a value already within
// bounds, decoding the written bytes back reproduces the same text the old
// verbatim behavior would have shown (every case this file's tests
// exercise), so this is observable only when a paste is actually clamped.
//
// Ragged pasted rows (a later row with fewer columns than the first): legacy
// computes its column count from the FIRST row only and indexes every row
// unconditionally at that width, which is an out-of-bounds
// QStringList::operator[] (UB / an assertion crash in debug Qt builds) for a
// shorter later row. This is a latent legacy safety bug, not a
// porting-relevant behavioral defect the spec tracks -- reproducing it would
// mean writing code with intentionally undefined behavior, which this port
// does not do. Instead, each cell is additionally skipped whenever
// `col >= pasted_rows[row].size()`; for any well-formed (non-ragged) paste --
// the only case any test here exercises -- this produces IDENTICAL output to
// legacy.
Result<EditPatch> apply_paste(bytes::ByteView rom_data, const MapElementSpec& spec, std::uint32_t x_size,
                              std::uint32_t y_size, std::span<const std::string_view> cell_text,
                              const SelectionRange& range, std::span<const std::vector<std::string_view>> pasted_rows,
                              int float_precision);

} // namespace fastecu::calibration
