#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// The "open a ROM off disk" half of open_subaru_rom_file. Does not perform
// definition matching.
//
// Deliberately separate from backup_rom rather than one function dispatching
// on whether preloaded bytes were supplied: the two modes share no arguments
// (a disk open has no backup handle, an already-loaded open has no file to
// read), and an emptiness test cannot distinguish "caller supplied no
// preloaded bytes" from "caller supplied a genuinely zero-length ROM".
Result<std::vector<std::uint8_t>> read_rom(std::string_view file_handle,
                                           IFileRepository& file_repository);

// The "already have the bytes" half: writes an in-hand ROM image (e.g. one
// just read off the ECU) to backup_handle. Returns void by design -- a failed
// backup must not fail the open, matching open_subaru_rom_file's own
// fire-and-forget backup save, which never inspected the write's result.
void backup_rom(std::span<const std::uint8_t> rom_data, std::string_view backup_handle,
                IFileRepository& file_repository);

// Byte width of one element. For Bloblist, derived from the first selection's
// hex-encoded value length (2 hex chars per byte) when `scaling` has one --
// this is what the legacy Qt loop does, not a guess. Falls back to
// definition::storage_byte_size(storage_type) (1 byte) when no scaling/
// selections are available to derive it from.
std::uint32_t element_byte_size(
    std::optional<definition::StorageType> storage_type,
    const definition::Scaling *scaling);

// One past the last byte touched by `count` elements of `element_width` bytes,
// laid out starting at `address` with the legacy start_position/interval
// stride: addr(j) = address + (start_position-1)*element_width +
// j*element_width*interval, for j in [0, count).
//
// Both degenerate inputs are handled rather than allowed to wrap in unsigned
// arithmetic, because both are reachable and both would otherwise produce a
// ~4 GB extent that makes validate_rom_size reject an otherwise fine ROM:
//   * count == 0 -- an empty run touches nothing, so the result is `address`.
//     The resolver rejects zero x_size/y_size/size today
//     (definition_resolver.cpp), but this is a public, separately-tested
//     function and must not depend on that.
//   * start_position == 0 -- out of domain for a 1-based position and rejected
//     by the resolver. Direct callers are still treated defensively as the
//     smallest legal value, i.e. offset 0.
std::uint64_t element_run_end(
    std::uint64_t address,
    std::uint32_t start_position,
    std::uint32_t interval,
    std::uint32_t element_width,
    std::uint32_t count);

// Every matched map's address, x-axis address, and y-axis address (each
// optional; absent addresses do not fail) must have its entire strided
// element run -- not just its base address -- fit within rom_byte_length.
// An address equal to rom_byte_length fails, since element_run_end always
// adds at least one element's width.
Status validate_rom_size(const definition::RomDefinition& rom_definition,
                         std::size_t rom_byte_length);

// Reproduces the sub_ecu_denso_mc68hc16y5_02 ROM-padding special case: inserts
// 0x8000 bytes of 0xFF at offset 0x20000 when flash_method starts with that
// protocol name and rom_data is under 190*1024 bytes. A no-op otherwise.
//
// Zero-extends up to 0x20000 first when rom_data is shorter. Qt's
// QByteArray::insert leaves such a gap "uninitialized" per its docs, so
// zero-fill is a disclosed deterministic choice, not a preserved legacy value.
//
// Consume-and-return of an owning buffer, deliberately: a by-reference or
// by-copy shape lets a caller pad a throwaway image and lose the result, which
// is exactly the regression PR #118's own final review caught. Callers write
//   rom = apply_flash_method_padding(std::move(rom), method);
std::vector<std::uint8_t> apply_flash_method_padding(
    std::vector<std::uint8_t> rom_data, std::string_view flash_method);

// ---------------------------------------------------------------------------
// Legacy map-value text contract (issue #135)
//
// Map consumers treat decoded values as STRINGS, not merely as numbers: the
// calibration tree, the map editor and the ROM writer all split and compare the
// text produced below. These four rules are therefore a compatibility boundary,
// not incidental formatting. Changing any of them changes what users see and
// what round-trips back into a ROM.
//
//   1. Numeric text follows QString::number(value, 'g', precision): trailing
//      zeros stripped, Qt's exponent thresholds, and -0 normalized to 0. The
//      portable implementation is format_like_qt_g in calibration_service.cpp,
//      which delegates to C's "%.*g". Qt's 'g' and C's %g agree across the
//      range these ROMs produce; that agreement is PINNED by
//      FormattingMatchesCapturedQtGroundTruth, which compares against captured
//      real Qt output rather than assuming compatibility. If %g is ever found
//      to diverge on a supported target, replace it with a portable formatter
//      matching this contract -- do not relax the contract to match %g.
//   2. Every value is followed by a comma, including the last: "v1,v2,...,vN,".
//   3. An absent or uncomputed axis is the single-space string " ", never "".
//   4. A map with no scaling takes the blank-expression path and emits
//      zero-valued cells; the raw value is NOT treated as identity-scaled.
//
// Rules 2-4 are restated at the specific declarations they govern below.
// ---------------------------------------------------------------------------

// One run of consecutive elements: a map's cells, or one axis's points. Built
// from either a CalibrationMap or an AxisDefinition -- the three call sites
// (map cells, X axis, Y axis) differ only in which fields they read.
//
// A non-owning view: `endian` and `from_byte` borrow from the RomDefinition
// (the map's scaling or the resolved axis fields) used to build the run, which
// always outlives the decode_scaled_values call. Never store one.
struct ElementRun
{
    std::uint64_t address{0};
    std::uint32_t count{0};
    std::uint32_t start_position{1};
    std::uint32_t interval{1};
    std::optional<definition::StorageType> storage_type;
    std::string_view endian;
    std::string_view from_byte{"x"};
    bool is_selectable{false};
};

// Decodes run.count consecutive elements laid out as
//   addr(j) = run.address + (run.start_position - 1) * width
//                         + j * width * run.interval
// and formats each through expression_evaluate(run.from_byte, x,
// float_precision) -- unless run.is_selectable, in which case the expression is
// not evaluated at all and every element is emitted as the formatted text of
// 0.0 (i.e. "0" at any precision), matching legacy's `value` staying
// default-initialized for a "Selectable" type.
//
// Output is "v1,v2,...,vN," -- a comma AFTER every value including the last,
// reproducing legacy's mapData.append(... + ",") verbatim. MapData consumers
// split on "," and rely on it; this is not a bug to fix.
//
// Endianness: non-float storage with run.endian == "little" reads
// least-significant byte first; anything else reads most-significant first.
// Float storage is always assembled big-endian, matching the legacy float
// branch on the supported little-endian hosts regardless of the endian field.
//
// Returns ErrorKind::Internal if any element's window would run past
// rom_data's end. Legacy indexed QByteArray::at() unchecked here, which
// asserts or reads out of bounds; this reports instead.
Result<std::string> decode_scaled_values(bytes::ByteView rom_data,
                                         const ElementRun& run,
                                         int float_precision);

// The StorageType::Bloblist branch: byte_count raw bytes from `address`,
// hex-encoded lowercase, two digits per byte, no scaling applied. byte_count
// comes from element_byte_size(storage_type, scaling).
Result<std::string> decode_bloblist_hex(bytes::ByteView rom_data,
                                        std::uint64_t address,
                                        std::uint32_t byte_count);

struct MapCellValues
{
    std::string map_data;
    // Default " ", not empty: legacy wrote a single space for an absent axis
    // and the calibration tree distinguishes the two.
    std::string x_axis_data{" "};
    std::string y_axis_data{" "};
    // Set when this one map's decode failed. The three fields above are then
    // left at their defaults, and the caller is expected to skip writing this
    // entry back and to log the error against this map's name and index. A
    // failed map never fails its siblings.
    std::optional<Error> error;
};
using MapCellValuesList = std::vector<MapCellValues>;

// One entry per rom_definition.maps, in the same order. Never fails as a
// whole -- per-map failures land in each entry's `error`.
//
// Map cells: decode_bloblist_hex when storage_type is Bloblist (width from
// element_byte_size, i.e. derived from the resolved scaling's first
// selection), else decode_scaled_values over x_size * y_size elements.
//
// X axis, only when x_size > 1: "Static X Axis"/"Static Y Axis" join the
// resolved axis's static_data comma-per-entry with a trailing comma;
// "X Axis", or "Y Axis" when map.type == "2D", decode from the axis's own
// address; any other type is left uncomputed at " ".
//
// Y axis, when y_size > 1: ALWAYS decoded, with no type branching at all.
// This asymmetry with the X axis is real legacy behavior, reproduced exactly.
Result<MapCellValuesList> compute_map_cell_values(
    const definition::RomDefinition& rom_definition,
    bytes::ByteView rom_data,
    int float_precision);

} // namespace fastecu::calibration
