#include "src/platform/desktop/windows/j2534/pe_bitness.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace
{

constexpr std::uint16_t kImageFileMachineI386 = 0x14C;

}

bool IsDll32Bit(const char *dll_path, bool& out32_bit)
{
    std::FILE *f = std::fopen(dll_path, "rb");
    if (!f)
    {
        return false;
    }

    std::array<unsigned char, 64> dos_header{};
    bool ok = std::fread(dos_header.data(), 1, dos_header.size(), f) == dos_header.size();
    if (ok && (dos_header[0] != 'M' || dos_header[1] != 'Z'))
    {
        ok = false;
    }

    std::int32_t pe_offset = 0;
    if (ok)
    {
        const std::uint32_t raw_offset =
            static_cast<std::uint32_t>(dos_header[0x3C]) | (static_cast<std::uint32_t>(dos_header[0x3D]) << 8) |
            (static_cast<std::uint32_t>(dos_header[0x3E]) << 16) | (static_cast<std::uint32_t>(dos_header[0x3F]) << 24);
        pe_offset = static_cast<std::int32_t>(raw_offset);
        ok = pe_offset >= 0;
    }

    std::array<unsigned char, 6> pe_and_machine{}; // "PE\0\0" + 2-byte Machine field
    if (ok)
    {
        ok = std::fseek(f, pe_offset, SEEK_SET) == 0 &&
             std::fread(pe_and_machine.data(), 1, pe_and_machine.size(), f) == pe_and_machine.size();
    }
    if (ok)
    {
        ok = pe_and_machine[0] == 'P' && pe_and_machine[1] == 'E' && pe_and_machine[2] == 0 && pe_and_machine[3] == 0;
    }

    std::fclose(f);
    if (!ok)
    {
        return false;
    }

    std::uint16_t machine = static_cast<std::uint16_t>(static_cast<std::uint32_t>(pe_and_machine[4]) |
                                                       (static_cast<std::uint32_t>(pe_and_machine[5]) << 8));
    out32_bit = (machine == kImageFileMachineI386);
    return true;
}
