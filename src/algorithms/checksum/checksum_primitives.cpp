#include "checksum_primitives.h"

#include <array>

namespace
{

constexpr std::array<std::uint32_t, 256> MakeCrcTable()
{
    std::array<std::uint32_t, 256> t = {};
    constexpr std::uint32_t kPolynomial = 0x5AA5A55A;

    for (std::uint32_t i = 0; i < t.size(); ++i)
    {
        std::uint32_t crc = 0;
        std::uint32_t c = i;

        for (std::uint32_t j = 0; j < 8; ++j)
        {
            if ((crc ^ c) & 0x00000001U)
            {
                crc = (crc >> 1) ^ kPolynomial;
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

constexpr std::array<std::uint32_t, 256> kCrcTable = MakeCrcTable();

} // namespace

namespace fastecu::checksum::internal
{

std::optional<ChecksumResult> RequireStartAtZero(const memory::MemoryView& rom)
{
    if (rom.Range().Start() == memory::FlashAddress{0})
    {
        return std::nullopt;
    }
    return ChecksumResult{.status = ChecksumResult::Status::kInvalidSize,
                          .rom_data = bytes::Bytes(rom.Data().begin(), rom.Data().end()),
                          .message = "ROM does not start at ECU address 0 as the checksum layout requires"};
}

void RebalanceU16Be(bytes::MutableByteView rom, std::size_t offset, std::uint16_t observed, std::uint16_t target)
{
    const std::uint16_t stored = bytes::ReadU16Be(rom, offset);
    bytes::WriteU16Be(rom, offset, static_cast<std::uint16_t>(stored + target - observed));
}

void RebalanceU32Be(bytes::MutableByteView rom, std::size_t offset, std::uint32_t observed, std::uint32_t target)
{
    const std::uint32_t stored = bytes::ReadU32Be(rom, offset);
    bytes::WriteU32Be(rom, offset, stored + target - observed);
}

} // namespace fastecu::checksum::internal

namespace fastecu::checksum
{

std::uint8_t CksAdd8(std::span<const std::uint8_t> data)
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

std::uint8_t NegatedSum8(bytes::ByteView data)
{
    return static_cast<std::uint8_t>(0x100 - bytes::Sum8(data));
}

std::uint32_t Crc32(bytes::ByteView data)
{
    std::uint32_t crc = 0xFFFFFFFF;
    for (const auto byte : data)
    {
        crc = kCrcTable[(crc ^ byte) & 0xff] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

std::uint32_t Crc32(const unsigned char *buf, std::uint32_t len)
{
    if (buf == nullptr)
    {
        return 0;
    }

    return Crc32(bytes::ByteView(reinterpret_cast<const bytes::Byte *>(buf), len));
}

} // namespace fastecu::checksum
