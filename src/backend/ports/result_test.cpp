#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
#include <gtest/gtest.h>

using fastecu::Error;
using fastecu::ErrorKind;
using fastecu::fail;
using fastecu::Result;
using fastecu::Status;

TEST(Result, HoldsValue)
{
    Result<int> r = 42;
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 42);
}

TEST(Result, HoldsError)
{
    Result<int> r = fail(ErrorKind::kTimeout, "read deadline");
    ASSERT_THAT(r, fastecu::testing::IsErr(ErrorKind::kTimeout));
    EXPECT_EQ(r.error().detail, "read deadline");
}

TEST(Status, VoidSuccessAndFailure)
{
    Status ok = {};
    EXPECT_TRUE(ok.has_value());
    Status bad = fail(ErrorKind::kDisconnected);
    ASSERT_THAT(bad, fastecu::testing::IsErr(ErrorKind::kDisconnected));
    EXPECT_TRUE(bad.error().detail.empty());
}

TEST(ErrorKind, StableSpellings)
{
    EXPECT_STREQ(fastecu::to_string(ErrorKind::kInvalidConfig), "InvalidConfig");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::kTimeout), "Timeout");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::kDisconnected), "Disconnected");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::kBadResponse), "BadResponse");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::kCancelled), "Cancelled");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::kUnsupported), "Unsupported");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::kInternal), "Internal");
}

TEST(Error, DefaultsToInternalKind)
{
    const Error error;
    EXPECT_EQ(error.kind, ErrorKind::kInternal);
    EXPECT_TRUE(error.detail.empty());
}
