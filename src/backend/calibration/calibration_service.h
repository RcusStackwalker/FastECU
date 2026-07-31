#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/file_repository.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

// Replaces FileActions::save_subaru_rom_file's body.
Status save_rom(std::span<const std::uint8_t> rom_data, std::string_view file_handle,
                IFileRepository& file_repository);

// Reads via file_repository when preloaded_bytes is empty (the "read from
// disk" case). When preloaded_bytes is non-empty (the "already have
// FullRomData" case, e.g. after an ECU read), backs it up to backup_handle
// via save_rom -- a backup-write failure does not fail the open, matching
// open_subaru_rom_file's own fire-and-forget backup save -- and returns
// preloaded_bytes unchanged. Does not perform definition matching.
Result<std::vector<std::uint8_t>> open_rom(std::string_view file_handle,
                                           std::span<const std::uint8_t> preloaded_bytes,
                                           std::string_view backup_handle,
                                           IFileRepository& file_repository);

// Replaces the AddressList/XScaleAddressList/YScaleAddressList bounds check:
// every matched map's address, x-axis address, and y-axis address (each
// optional; absent addresses do not fail) must not exceed rom_byte_length.
// Uses the legacy comparison's exact ">" (not ">="), so an address equal to
// rom_byte_length passes.
Status validate_rom_size(const definition::RomDefinition& rom_definition,
                         std::size_t rom_byte_length);

} // namespace fastecu::calibration
