#include "src/platform/desktop/windows/j2534/pe_bitness.h"

#include <gtest/gtest.h>
#include <cstdlib>

class PeBitness : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        x86Path = std::getenv("PE_BITNESS_X86_FIXTURE");
        x64Path = std::getenv("PE_BITNESS_X64_FIXTURE");
    }

    static const char *x86Path;
    static const char *x64Path;
};

const char *PeBitness::x86Path = nullptr;
const char *PeBitness::x64Path = nullptr;

TEST_F(PeBitness, DetectsBothArchitecturesAndRejectsMissingFile)
{
    ASSERT_NE(x86Path, nullptr) << "set PE_BITNESS_X86_FIXTURE";
    ASSERT_NE(x64Path, nullptr) << "set PE_BITNESS_X64_FIXTURE";

    bool is32 = false;
    ASSERT_TRUE(isDll32Bit(x86Path, is32)) << "isDll32Bit should succeed on a valid PE file";
    EXPECT_TRUE(is32) << "x86 fixture should be detected as 32-bit";

    bool is32b = true;
    ASSERT_TRUE(isDll32Bit(x64Path, is32b)) << "isDll32Bit should succeed on a valid PE file";
    EXPECT_FALSE(is32b) << "x64 fixture should be detected as 64-bit";

    bool unused = false;
    EXPECT_FALSE(isDll32Bit("Z:\\does\\not\\exist.dll", unused)) << "missing file should fail cleanly";
}
