#include "pe_bitness.h"

#include <cassert>
#include <cstdio>

int main(int argc, char **argv)
{
    assert(argc == 3 && "usage: pe_bitness_test <x86-binary> <x64-binary>");
    const char *x86Path = argv[1];
    const char *x64Path = argv[2];

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
