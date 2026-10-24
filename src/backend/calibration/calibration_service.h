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
#include "src/backend/calibration/element_run.h"
#include "src/backend/calibration/definition_bytes.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// The "open a ROM off disk" half of open_subaru_rom_file. Does not perform
// definition matching.
//
// Deliberately separate from BackupRom rather than one function dispatching
// on whether preloaded bytes were supplied: the two modes share no arguments
// (a disk open has no backup handle, an already-loaded open has no file to
// read), and an emptiness test cannot distinguish "caller supplied no
// preloaded bytes" from "caller supplied a genuinely zero-length ROM".
Result<std::vector<std::uint8_t>> ReadRom(std::string_view file_handle, IFileRepository& file_repository);

// The "already have the bytes" half: writes an in-hand ROM image (e.g. one
// just read off the ECU) to `backup_handle`. Returns void by design -- a failed
// backup must not fail the open, matching open_subaru_rom_file's own
// fire-and-forget backup save, which never inspected the write's result.
void BackupRom(std::span<const std::uint8_t> rom_data, std::string_view backup_handle,
               IFileRepository& file_repository);

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

Result<NumericRun> DecodeNumericRun(const DefinitionBytes& rom, const ElementRun& run);
Result<DecodedMap> DecodeCalibrationMap(const definition::RomDefinition& definition,
                                        const definition::CalibrationMap& map, const DefinitionBytes& rom);

} // namespace fastecu::calibration
