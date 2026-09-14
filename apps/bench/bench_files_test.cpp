#include "src/backend/ports/testing/result_matchers.h"
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

    ASSERT_THAT(files.save(path, written), fastecu::testing::IsOk());
    const Result<bytes::Bytes> read = files.load(path);

    ASSERT_THAT(read, fastecu::testing::IsOkAnd(written));
}

TEST(BenchFiles, LoadsAnEmptyFileAsNoBytes)
{
    const std::string path = tempPath("empty.bin");
    {
        const std::ofstream create(path, std::ios::binary | std::ios::trunc);
    }
    BenchFiles files;

    const Result<bytes::Bytes> read = files.load(path);

    ASSERT_THAT(read, fastecu::testing::IsOk());
    EXPECT_TRUE(read->empty());
}

TEST(BenchFiles, LoadReportsAMissingFileAsInvalidConfig)
{
    BenchFiles files;

    const Result<bytes::Bytes> read = files.load(tempPath("absent.bin"));

    ASSERT_THAT(read, fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

TEST(BenchFiles, SaveReportsAnUnwritablePathAsInternal)
{
    BenchFiles files;

    const Status saved = files.save(tempPath("no_such_dir/out.bin"), bytes::Bytes{0x01});

    ASSERT_THAT(saved, fastecu::testing::IsErr(ErrorKind::Internal));
}

} // namespace
} // namespace fastecu::bench
