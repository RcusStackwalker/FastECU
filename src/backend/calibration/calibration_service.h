#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "src/backend/definition/definition_model.h"
#include "src/backend/ports/result.h"

namespace fastecu::calibration
{

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
// j*element_width*interval, for j in [0, count). `count` is guaranteed >= 1
// by the resolver (x_size/y_size/size are never zero), so no count == 0
// special case is needed. `start_position == 0` underflows (start_position-1)
// to a huge offset -- this matches the legacy code's own behavior on an
// unsigned start_position in file_actions.cpp, so it's preserved as-is.
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

} // namespace fastecu::calibration
