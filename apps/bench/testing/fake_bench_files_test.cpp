#include "src/algorithms/protocol/testing/byte_matchers.h"
#include "src/backend/ports/testing/result_matchers.h"
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

    EXPECT_THAT(files.load("in.bin").value(), test_bytes::BytesEq((bytes::Bytes{1, 2, 3})));
    EXPECT_THAT(files.load("missing.bin"), ::testing::Not(fastecu::testing::IsOk()));
    ASSERT_THAT(files.save("out.bin", bytes::Bytes{4, 5}), fastecu::testing::IsOk());
    EXPECT_THAT(files.saved["out.bin"], test_bytes::BytesEq((bytes::Bytes{4, 5})));
}

TEST(FakeBenchFiles, MissingFileReportsInvalidConfig)
{
    FakeBenchFiles files;

    ASSERT_THAT(files.load("missing.bin"), fastecu::testing::IsErr(ErrorKind::InvalidConfig));
}

} // namespace
} // namespace fastecu::bench::testing
