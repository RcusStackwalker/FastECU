#include "pe_bitness.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    const char *x86Path = std::getenv("PE_BITNESS_X86_FIXTURE");
    const char *x64Path = std::getenv("PE_BITNESS_X64_FIXTURE");
    if ((!x86Path || !x64Path) && argc == 3)
    {
        x86Path = argv[1];
        x64Path = argv[2];
    }
    assert(x86Path && x64Path &&
           "set PE_BITNESS_X86_FIXTURE/PE_BITNESS_X64_FIXTURE, or pass <x86-binary> <x64-binary> as args");

    bool is32 = false;
    assert(isDll32Bit(x86Path, is32) && "isDll32Bit should succeed on a valid PE file");
    assert(is32 && "x86 fixture should be detected as 32-bit");

    bool is32b = true;
    assert(isDll32Bit(x64Path, is32b) && "isDll32Bit should succeed on a valid PE file");
    assert(!is32b && "x64 fixture should be detected as 64-bit");

    bool unused = false;
    assert(!isDll32Bit("Z:\\does\\not\\exist.dll", unused) && "missing file should fail cleanly");

    std::printf("All pe_bitness tests passed.\n");
    return 0;
}
