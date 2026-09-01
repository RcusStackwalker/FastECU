#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"

namespace fastecu::service_functions
{

// The nine values the operator supplies. legacy :162-202 prompts for these
// with bounds 0-255, and 0-65535 for AWD torque -- exactly these types, so the
// bounds are enforced by the value model rather than a runtime range check.
struct TcuParameterValues
{
    bytes::Byte correction_1to2{};             // DC,   written to 0x16e
    bytes::Byte correction_2to3{};             // HLRC, written to 0x16d
    bytes::Byte correction_3to4{};             // IC,   written to 0x16c
    bytes::Byte correction_4to5{};             // FB,   written to 0x16f
    bytes::Byte correction_forward_brake{};    // 0x1bc
    bytes::Byte correction_four_wheel_drive{}; // 0x1bd
    bytes::Byte correction_line_pressure{};    // 0x1be
    bytes::Byte temperature_basis{};           // 0x1bf
    std::uint16_t torque_correction_awd{};     // 0x170 (high) / 0x171 (low)

    bool operator==(const TcuParameterValues&) const = default;
};

// One SSM 0xB8 write-address exchange. Rows are addresses, not parameters:
// AWD torque occupies two, and the trailing commit pair has no parameter.
struct TcuParameterWrite
{
    // 24-bit. Parameter rows are 0x0001xx; the legacy commit payload is
    // B8 00 00 EC 55/AA (legacy :453-479).
    std::uint32_t address;
    bytes::Byte value;

    bool operator==(const TcuParameterWrite&) const = default;
};

inline constexpr std::size_t kTcuParameterWriteCount = 12;

// The twelve writes in legacy wire order. The legacy performs the same twelve
// but only its first frame is well-formed (:215 reassigns `output` to the
// framed array; :237 onward mutate indices that then address the SSM length
// byte and service ID, and re-frame an already-framed buffer). Composing each
// payload from this table is the correction. The final two rows preserve the
// legacy's distinct 0x0000ec commit address.
std::array<TcuParameterWrite, kTcuParameterWriteCount> tcu_parameter_writes(const TcuParameterValues& values);

} // namespace fastecu::service_functions
