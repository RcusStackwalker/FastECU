#include "pe_bitness.h"

#include <cstdint>
#include <cstdio>

namespace
{

constexpr std::uint16_t kImageFileMachineI386 = 0x14C;

}

bool isDll32Bit(const char *dllPath, bool& out32Bit)
{
    std::FILE *f = std::fopen(dllPath, "rb");
    if (!f)
        return false;

    unsigned char dosHeader[64];
    bool ok = std::fread(dosHeader, 1, sizeof(dosHeader), f) == sizeof(dosHeader);
    if (ok && !(dosHeader[0] == 'M' && dosHeader[1] == 'Z'))
        ok = false;

    std::int32_t peOffset = 0;
    if (ok)
    {
        peOffset = static_cast<std::int32_t>(
            dosHeader[0x3C] | (dosHeader[0x3D] << 8) | (dosHeader[0x3E] << 16) | (dosHeader[0x3F] << 24));
        ok = peOffset >= 0;
    }

    unsigned char peAndMachine[6]; // "PE\0\0" + 2-byte Machine field
    if (ok)
    {
        ok = std::fseek(f, peOffset, SEEK_SET) == 0 &&
             std::fread(peAndMachine, 1, sizeof(peAndMachine), f) == sizeof(peAndMachine);
    }
    if (ok)
    {
        ok = peAndMachine[0] == 'P' && peAndMachine[1] == 'E' && peAndMachine[2] == 0 && peAndMachine[3] == 0;
    }

    std::fclose(f);
    if (!ok)
        return false;

    std::uint16_t machine = static_cast<std::uint16_t>(peAndMachine[4] | (peAndMachine[5] << 8));
    out32Bit = (machine == kImageFileMachineI386);
    return true;
}
