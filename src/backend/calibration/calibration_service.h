#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/definition/definition_model.h"
#include "src/backend/calibration/decoded_map.h"
#include "src/backend/calibration/definition_bytes.h"
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
Result<std::vector<std::uint8_t>> ReadRom(std::string_view file_handle, IFileRepository& file_repository);

// The "already have the bytes" half: writes an in-hand ROM image (e.g. one
// just read off the ECU) to backup_handle. Returns void by design -- a failed
// backup must not fail the open, matching open_subaru_rom_file's own
// fire-and-forget backup save, which never inspected the write's result.
void BackupRom(std::span<const std::uint8_t> rom_data, std::string_view backup_handle,
               IFileRepository& file_repository);

// Byte width of one element. For Bloblist, derived from the first selection's
// hex-encoded value length (2 hex chars per byte) when `scaling` has one --
// this is what the legacy Qt loop does, not a guess. Falls back to
// definition::storage_byte_size(storage_type) (1 byte) when no scaling/
// selections are available to derive it from.
std::uint32_t ElementByteSize(std::optional<definition::StorageType> storage_type, const definition::Scaling *scaling);

// Returns UINT64_MAX on layout arithmetic overflow.
// One past the last byte touched by `count` elements of `element_width` bytes,
// laid out starting at `address` with the legacy start_position/interval
// stride: addr(j) = address + (start_position-1)*element_width +
// j*element_width*interval, for j in [0, count).
//
// Both degenerate inputs are handled rather than allowed to wrap in unsigned
// arithmetic, because both are reachable and both would otherwise produce a
// ~4 GB extent that makes the structural placement check reject a fine map:
//   * count == 0 -- an empty run touches nothing, so the result is `address`.
//     The resolver rejects zero x_size/y_size/size today
//     (definition_resolver.cpp), but this is a public, separately-tested
//     function and must not depend on that.
//   * start_position == 0 -- out of domain for a 1-based position and rejected
//     by the resolver. Direct callers are still treated defensively as the
//     smallest legal value, i.e. offset 0.
std::uint64_t ElementRunEnd(std::uint64_t address, std::uint32_t start_position, std::uint32_t interval,
                            std::uint32_t element_width, std::uint32_t count);

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
// always outlives the decode_numeric_run call. Never store one.
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

Result<NumericRun> DecodeNumericRun(const DefinitionBytes& rom, const ElementRun& run);
Result<DecodedMap> DecodeCalibrationMap(const definition::RomDefinition& definition,
                                        const definition::CalibrationMap& map, const DefinitionBytes& rom);

} // namespace fastecu::calibration
