#pragma once

#include <cstdint>

#include "src/algorithms/protocol/bytes.h"

// Shared layout checks and signed storage interpretation for calibration decoding
// and encoding.
//
// Internal to //src/backend/calibration. Not part of any public API.
namespace fastecu::calibration::internal
{

// Sign-extends a `width`-byte raw value to a signed 32-bit value.
std::int32_t sign_extend(std::uint32_t raw, std::uint32_t width);

// Overflow-checked arithmetic. Return false and leave `result` unspecified on
// overflow rather than wrapping, so a bad definition cannot silently produce a
// ~4 GB extent.
bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result);
bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result);

// True when [address, address + width) lies wholly inside `data`.
bool byte_window_fits(bytes::ByteView data, std::uint64_t address, std::uint64_t width);

} // namespace fastecu::calibration::internal
