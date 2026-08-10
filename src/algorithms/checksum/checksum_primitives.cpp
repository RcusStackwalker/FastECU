#include "checksum_primitives.h"

#include <array>

namespace
{

constexpr std::array<std::uint32_t, 256> makeCrcTable()
{
    std::array<std::uint32_t, 256> t = {};
    constexpr std::uint32_t polynomial = 0x5AA5A55A;

    for (std::uint32_t i = 0; i < t.size(); ++i)
    {
        std::uint32_t crc = 0;
        std::uint32_t c = i;

        for (std::uint32_t j = 0; j < 8; ++j)
        {
            if ((crc ^ c) & 0x00000001U)
            {
                crc = (crc >> 1) ^ polynomial;
            }
            else
            {
                crc = crc >> 1;
            }
            c = c >> 1;
        }
        t[i] = crc;
    }

    return t;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = makeCrcTable();

} // namespace

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

std::uint8_t checksum8(bytes::ByteView data, bool dec0x100)
{
    const bytes::Byte sum = bytes::sum8(data);
    return dec0x100 ? static_cast<std::uint8_t>(0x100 - sum) : sum;
}

std::uint32_t crc32(bytes::ByteView data)
{
    std::uint32_t crc = 0xFFFFFFFF;
    for (const auto byte : data)
    {
        crc = kCrcTable[(crc ^ byte) & 0xff] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

std::uint32_t crc32(const unsigned char *buf, std::uint32_t len)
{
    if (buf == nullptr)
    {
        return 0;
    }

    return crc32(bytes::ByteView(reinterpret_cast<const bytes::Byte *>(buf), len));
}

} // namespace fastecu::checksum
