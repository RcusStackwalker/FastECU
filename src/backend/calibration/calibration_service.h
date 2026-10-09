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
// ~4 GB extent that makes validate_rom_size reject an otherwise fine ROM:
//   * count == 0 -- an empty run touches nothing, so the result is `address`.
//     The resolver rejects zero x_size/y_size/size today
//     (definition_resolver.cpp), but this is a public, separately-tested
//     function and must not depend on that.
//   * start_position == 0 -- out of domain for a 1-based position and rejected
//     by the resolver. Direct callers are still treated defensively as the
//     smallest legal value, i.e. offset 0.
std::uint64_t ElementRunEnd(std::uint64_t address, std::uint32_t start_position, std::uint32_t interval,
                            std::uint32_t element_width, std::uint32_t count);

// Every matched map's address, x-axis address, and y-axis address (each
// optional; absent addresses do not fail) must have its entire strided
// element run -- not just its base address -- fit within rom_byte_length.
// An address equal to rom_byte_length fails, since element_run_end always
// adds at least one element's width.
Status ValidateRomSize(const definition::RomDefinition& rom_definition, std::size_t rom_byte_length);

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
std::vector<std::uint8_t> ApplyFlashMethodPadding(std::vector<std::uint8_t> rom_data, std::string_view flash_method);

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

Result<NumericRun> DecodeNumericRun(bytes::ByteView rom, const ElementRun& run);
Result<DecodedMap> DecodeCalibrationMap(const definition::RomDefinition& definition,
                                        const definition::CalibrationMap& map, bytes::ByteView rom);

} // namespace fastecu::calibration
