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

// Reproduces file_actions.cpp's sub_ecu_denso_mc68hc16y5_02 ROM-padding
// special case: inserts 0x8000 bytes of 0xFF at offset 0x20000 when
// flash_method starts with "sub_ecu_denso_mc68hc16y5_02" and rom_data is
// under 190*1024 bytes; a no-op otherwise. If rom_data is shorter than
// the 0x20000 insertion point, it is first zero-extended up to that
// point (Qt's own auto-extend-on-insert semantics leave that gap
// "uninitialized" per its docs, so zero-fill is a disclosed, deterministic
// choice, not a preserved legacy value). Callers take ownership:
// rom_data = apply_flash_method_padding(std::move(rom_data), flash_method);
std::vector<std::uint8_t> apply_flash_method_padding(
    std::vector<std::uint8_t> rom_data, std::string_view flash_method);

} // namespace fastecu::calibration
