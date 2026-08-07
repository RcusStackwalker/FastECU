#include "checksum_primitives.h"

namespace fastecu::checksum::internal
{

void rebalanceU16Be(bytes::MutableByteView rom, std::size_t offset,
                    std::uint16_t observed, std::uint16_t target)
{
    const std::uint16_t stored = bytes::readU16Be(rom, offset);
    bytes::writeU16Be(rom, offset, static_cast<std::uint16_t>(stored + target - observed));
}

void rebalanceU32Be(bytes::MutableByteView rom, std::size_t offset,
                    std::uint32_t observed, std::uint32_t target)
{
    const std::uint32_t stored = bytes::readU32Be(rom, offset);
    bytes::writeU32Be(rom, offset, stored + target - observed);
}

} // namespace fastecu::checksum::internal

namespace fastecu::checksum
{

std::uint8_t cks_add8(std::span<const std::uint8_t> data)
{
    std::uint16_t sum = 0;
    for (std::uint8_t byte : data)
    {
        sum += byte;
        if (sum & 0x100)
        {
            sum += 1;
        }
        sum = static_cast<std::uint8_t>(sum);
    }
    return static_cast<std::uint8_t>(sum);
}

} // namespace fastecu::checksum
