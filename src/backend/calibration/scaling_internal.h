#pragma once

#include <cstdint>
#include <string>

#include "src/algorithms/protocol/bytes.h"

// Helpers shared by the decode side (calibration_service.cpp) and the encode
// side (map_edit.cpp). They live here rather than in either .cpp's anonymous
// namespace because the encode/decode round trip is only meaningful if both
// sides format and sign-extend identically.
//
// Internal to //src/backend/calibration. Not part of any public API.
namespace fastecu::calibration::internal
{

// Qt's QString::number(double) formatting, reproduced: 'g' format, `precision`
// significant digits, trailing zeros stripped.
std::string format_like_qt_g(double value, int precision);

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
