#include "src/platform/desktop/windows/j2534/pe_bitness.h"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>

namespace
{
const char *x86Path = nullptr;
const char *x64Path = nullptr;
} // namespace

TEST(PeBitness, DetectsBothArchitecturesAndRejectsMissingFile)
{
    ASSERT_TRUE(x86Path && x64Path &&
                "set PE_BITNESS_X86_FIXTURE/PE_BITNESS_X64_FIXTURE, or pass <x86-binary> <x64-binary> as args");

    bool is32 = false;
    ASSERT_TRUE(isDll32Bit(x86Path, is32) && "isDll32Bit should succeed on a valid PE file");
    ASSERT_TRUE(is32 && "x86 fixture should be detected as 32-bit");

    bool is32b = true;
    ASSERT_TRUE(isDll32Bit(x64Path, is32b) && "isDll32Bit should succeed on a valid PE file");
    ASSERT_TRUE(!is32b && "x64 fixture should be detected as 64-bit");

    bool unused = false;
    ASSERT_TRUE(!isDll32Bit("Z:\\does\\not\\exist.dll", unused) && "missing file should fail cleanly");

    std::printf("All pe_bitness tests passed.\n");
}

int run_pe_bitness_tests(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    x86Path = std::getenv("PE_BITNESS_X86_FIXTURE");
    x64Path = std::getenv("PE_BITNESS_X64_FIXTURE");
    if ((!x86Path || !x64Path) && argc == 3)
    {
        x86Path = argv[1];
        x64Path = argv[2];
    }
    return RUN_ALL_TESTS();
}
