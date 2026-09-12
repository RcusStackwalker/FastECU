#include "apps/bench/bench_files.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>

namespace fastecu::bench
{
namespace
{

std::string tempPath(std::string_view name)
{
    const char *const dir = std::getenv("TEST_TMPDIR");
    return std::string(dir != nullptr ? dir : ".") + "/" + std::string(name);
}

TEST(BenchFiles, RoundTripsBytesThroughTheFilesystem)
{
    const std::string path = tempPath("round_trip.bin");
    const bytes::Bytes written{0x00, 0x01, 0xFE, 0xFF, 0x7F};
    BenchFiles files;

    ASSERT_TRUE(files.save(path, written).has_value());
    const Result<bytes::Bytes> read = files.load(path);

    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(*read, written);
}

TEST(BenchFiles, LoadsAnEmptyFileAsNoBytes)
{
    const std::string path = tempPath("empty.bin");
    {
        const std::ofstream create(path, std::ios::binary | std::ios::trunc);
    }
    BenchFiles files;

    const Result<bytes::Bytes> read = files.load(path);

    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(read->empty());
}

TEST(BenchFiles, LoadReportsAMissingFileAsInvalidConfig)
{
    BenchFiles files;

    const Result<bytes::Bytes> read = files.load(tempPath("absent.bin"));

    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().kind, ErrorKind::InvalidConfig);
}

TEST(BenchFiles, SaveReportsAnUnwritablePathAsInternal)
{
    BenchFiles files;

    const Status saved = files.save(tempPath("no_such_dir/out.bin"), bytes::Bytes{0x01});

    ASSERT_FALSE(saved.has_value());
    EXPECT_EQ(saved.error().kind, ErrorKind::Internal);
}

} // namespace
} // namespace fastecu::bench
