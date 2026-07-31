#pragma once
#include <charconv>
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

// Decodes `count` consecutive cells starting at base_address (map cells, or
// one axis's points), honoring storage_type/endian/interval/start_position,
// and formats each through expression_evaluate(from_byte_expression, x,
// float_precision) unless is_selectable is true (every value emitted as
// "0", matching legacy's value staying at its default-initialized 0.0 when
// TypeList/XScaleTypeList/YScaleTypeList == "Selectable"). Output is
// "v1,v2,...,vN," -- comma AFTER every value including the last,
// reproducing file_actions.cpp's mapData.append(...+",") verbatim (not a
// bug to fix). apply_wrx02_wraparound reproduces the flash_method ==
// "wrx02" quirk: subtracts 0x8000 from a computed byte address that
// exceeds rom_data.size(). start_position/interval are parsed as hex
// (0 on empty/unparseable input) -- matching legacy's
// QString::toUInt(&status, 16) on these two fields specifically (unlike
// map/axis addresses, which are already numeric in RomDefinition and need
// no parsing). The (start_position - 1) computation underflows via
// well-defined uint32_t wraparound when start_position == 0, exactly
// matching legacy -- not "fixed". Returns fail(ErrorKind::Internal, ...)
// if a computed byte address would read past rom_data.size() (legacy
// relies on QByteArray::at()'s UB/assert here; this bounds-checks
// explicitly instead of reproducing that).
//
// DISCLOSED BEHAVIOR FIX, not byte-for-byte legacy parity -- confirmed by
// compiling the legacy union-based decode loop directly: for any
// non-float storage type with endian=="little", the legacy code fills a
// union member that the value-selection code below it never reads for
// non-float types -- it reads separate scalar accumulators instead, which
// stay at their per-cell reset value of 0 the entire time. Net effect in
// the shipped app today: every endian=="little" non-float map/axis always
// evaluates from_byte_expression at x=0, a constant, ignoring actual ROM
// bytes. This implementation instead decodes correctly and symmetrically
// for both float and non-float types: endian=="little" reads bytes with
// the first file byte as least significant (true little-endian);
// anything else (including "big", empty, or any other value, matching
// legacy's == "little" / else split) reads the first file byte as most
// significant (standard big-endian -- unchanged from what already works
// correctly today). Any real-world endian=="little" map or axis will show
// different (correct) values after this lands.
//
// DISCLOSED BEHAVIOR FIX #2: legacy's signed-integer accumulation path
// relies on QByteArray::at()'s return type (char, implementation-defined
// signedness) with no explicit unsigned cast, so an intermediate byte with
// its high bit set gets platform-dependently sign-extended before the
// shift-add on hosts where char defaults to signed (typical x86_64
// Linux/Windows; this project's primary arm64 macOS host defaults to
// unsigned char, where legacy's behavior already happens to be correct).
// This implementation explicitly masks each byte to uint8_t during
// accumulation and sign-extends only the final assembled value based on
// its top bit -- well-defined on every platform, identical to today's
// arm64 macOS behavior, and a disclosed correction on signed-char hosts.
Result<std::string> decode_scaled_values(
    std::span<const std::uint8_t> rom_data,
    std::uint64_t base_address,
    std::uint32_t count,
    std::string_view start_position,
    std::string_view interval,
    std::string_view storage_type,
    std::string_view endian,
    std::string_view from_byte_expression,
    bool is_selectable,
    bool apply_wrx02_wraparound,
    int float_precision);

} // namespace fastecu::calibration
