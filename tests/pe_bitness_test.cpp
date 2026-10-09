#include "src/platform/desktop/windows/j2534/pe_bitness.h"

#include <gtest/gtest.h>
#include <cstdlib>

class PeBitness : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        x86_path_ = std::getenv("PE_BITNESS_X86_FIXTURE");
        x64_path_ = std::getenv("PE_BITNESS_X64_FIXTURE");
    }

    static const char *x86_path_;
    static const char *x64_path_;
};

const char *PeBitness::x86_path_ = nullptr;
const char *PeBitness::x64_path_ = nullptr;

TEST_F(PeBitness, DetectsBothArchitecturesAndRejectsMissingFile)
{
    ASSERT_NE(x86_path_, nullptr) << "set PE_BITNESS_X86_FIXTURE";
    ASSERT_NE(x64_path_, nullptr) << "set PE_BITNESS_X64_FIXTURE";

    bool is32 = false;
    ASSERT_TRUE(isDll32Bit(x86_path_, is32)) << "isDll32Bit should succeed on a valid PE file";
    EXPECT_TRUE(is32) << "x86 fixture should be detected as 32-bit";

    bool is32b = true;
    ASSERT_TRUE(isDll32Bit(x64_path_, is32b)) << "isDll32Bit should succeed on a valid PE file";
    EXPECT_FALSE(is32b) << "x64 fixture should be detected as 64-bit";

    bool unused = false;
    EXPECT_FALSE(isDll32Bit("Z:\\does\\not\\exist.dll", unused)) << "missing file should fail cleanly";
}
