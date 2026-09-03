#pragma once

#include <cstdint>
#include <optional>
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
// `legacy_byte_order` carries the same two measured, DIFFERENT non-float
// multi-byte write orders as encode_scaled_value (see its comment for the
// measurements); floats are unaffected by the flag in either mode, always
// written big-endian-in-ROM.
Result<std::vector<std::uint8_t>> write_raw_element(const MapElementSpec& spec, std::int64_t raw,
                                                    bool legacy_byte_order);

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
// `legacy_byte_order` selects between two measured, DIFFERENT non-float
// multi-byte write orders -- mirroring how element_byte_address carries both
// wrx02 predicates behind `for_write` until the fix wave reconciles them:
//   - false: writes the byte order spec.endian's label claims (matching
//     decode_scaled_values, the correct decoder). This is the inverse of
//     read_raw_element's *correct* half (the unsigned/float paths), not of
//     its signed-multi-byte defect; see
//     PinnedDefect_SignedMultiByteDoesNotRoundTrip* in map_edit_test.cpp for
//     the resulting divergence on signed 16/32-bit storage.
//   - true: reproduces legacy set_rom_data_value's write order, which is
//     INVERTED relative to its own endian label (raw 0x1234 labeled "big"
//     writes [0x34, 0x12]) -- see
//     PinnedDefect_WriteOrderDivergesFromLegacyBecauseLegacyIsAlsoByteSwapped.
// Float storage is unaffected by this flag in either mode: floats are always
// written big-endian-in-ROM, matching decode_scaled_values and
// read_raw_element's float handling.
Result<std::vector<std::uint8_t>> encode_scaled_value(const MapElementSpec& spec, double display_value,
                                                      int float_precision, bool legacy_byte_order);

// Display helpers: pure formatting computations pulled out of
// get_mapvalue_decimal_count and get_map_cell_colors (menu_actions.cpp).
// Neither touches ROM data; both feed how a value is rendered in the map
// grid widget.

// How many decimal places to render a cell's value with, reproducing
// get_mapvalue_decimal_count. `value_format` is a RomRaider-style format
// string (e.g. "0.00"); legacy counts the `'0'` characters in the segment
// between the FIRST and SECOND '.' -- QString::split(".").at(1), not
// everything after the first '.' -- so a format string with more than one
// '.' only has its first fractional segment counted. No '.' at all returns
// 0.
int map_value_decimal_count(std::string_view value_format);

// The HSV hue for a map cell's background color, reproducing the
// color_scale/color_value arithmetic in get_map_cell_colors. Maps
// `min_value` to 0 and `max_value` to the top of the range (210/360),
// clamping negative results (values below `min_value`) to 0. The
// QColor::setHsvF/getRgbF conversion to a packed RGB int stays in
// menu_actions.cpp, since that's presentation, not logic.
//
// PinnedDefect_EqualColorBoundsProduceNonFiniteHue (map_edit_test.cpp): when
// `min_value == max_value`, the division by zero in this formula produces a
// non-finite hue -- preserved verbatim from legacy. Display-only (no ROM
// write involved), so it does not join the write-path defects the spec
// tracks for the 6b-4 fix wave.
double map_cell_color_scale(double value, double min_value, double max_value);

} // namespace fastecu::calibration
