#include "apps/bench/testing/fake_bench_files.h"

#include <gtest/gtest.h>

namespace fastecu::bench::testing
{
namespace
{

TEST(FakeBenchFiles, RoundTripsLoadAndSave)
{
    FakeBenchFiles files;
    files.contents["in.bin"] = bytes::Bytes{1, 2, 3};

    EXPECT_EQ(files.load("in.bin").value(), (bytes::Bytes{1, 2, 3}));
    EXPECT_FALSE(files.load("missing.bin").has_value());
    ASSERT_TRUE(files.save("out.bin", bytes::Bytes{4, 5}).has_value());
    EXPECT_EQ(files.saved["out.bin"], (bytes::Bytes{4, 5}));
}

TEST(FakeBenchFiles, MissingFileReportsInvalidConfig)
{
    FakeBenchFiles files;

    const auto result = files.load("missing.bin");

    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, ErrorKind::InvalidConfig);
}

} // namespace
} // namespace fastecu::bench::testing
