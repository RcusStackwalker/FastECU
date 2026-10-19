#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"

namespace fastecu::flash::testing
{
enum class N83mPattern
{
    kForward,
    kReverse
};
struct N83mPayload
{
    bytes::Bytes plain;
    bytes::Bytes wire;
};

inline N83mPayload MakeN83mPayload(std::size_t image_size, N83mPattern pattern)
{
    // DensoIso15765CanCommon.EncryptMatchesTheLegacyVector and
    // SsmProtocolCore.CalculatePayloadMatchesTheCommonDensoVector.
    constexpr std::array<std::array<bytes::Byte, 4>, 4> kPlain{
        {{0x00, 0x01, 0x02, 0x03}, {0xFC, 0xFD, 0xFE, 0xFF}, {0x00, 0x11, 0x22, 0x33}, {0x44, 0x55, 0x66, 0x77}}};
    constexpr std::array<std::array<bytes::Byte, 4>, 4> kWire{
        {{0xE0, 0xD3, 0x85, 0x2B}, {0xC5, 0xFE, 0x4B, 0x39}, {0xED, 0x9F, 0xD9, 0x31}, {0xAF, 0xAC, 0xD5, 0x94}}};
    N83mPayload payload;
    payload.plain.reserve(image_size);
    payload.wire.reserve(image_size);
    for (std::size_t offset = 0; offset < image_size; offset += 4)
    {
        const auto page = static_cast<std::uint32_t>(offset / 256);
        const auto word = (offset % 256) / 4;
        const auto shift = 2 * (pattern == N83mPattern::kForward ? word % 16 : 15 - word % 16);
        const auto digit = (page >> shift) & 3U;
        const auto index = pattern == N83mPattern::kForward ? digit : 3U - digit;
        payload.plain.insert(payload.plain.end(), kPlain[index].begin(), kPlain[index].end());
        payload.wire.insert(payload.wire.end(), kWire[index].begin(), kWire[index].end());
    }
    return payload;
}
} // namespace fastecu::flash::testing
