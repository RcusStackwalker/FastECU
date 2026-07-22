#include "src/backend/ports/result.h"
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
    Result<int> r = fail(ErrorKind::Timeout, "read deadline");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().kind, ErrorKind::Timeout);
    EXPECT_EQ(r.error().detail, "read deadline");
}

TEST(Status, VoidSuccessAndFailure)
{
    Status ok = {};
    EXPECT_TRUE(ok.has_value());
    Status bad = fail(ErrorKind::Disconnected);
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().kind, ErrorKind::Disconnected);
    EXPECT_TRUE(bad.error().detail.empty());
}

TEST(ErrorKind, StableSpellings)
{
    EXPECT_STREQ(fastecu::to_string(ErrorKind::InvalidConfig), "InvalidConfig");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::Timeout), "Timeout");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::Disconnected), "Disconnected");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::BadResponse), "BadResponse");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::Cancelled), "Cancelled");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::Unsupported), "Unsupported");
    EXPECT_STREQ(fastecu::to_string(ErrorKind::Internal), "Internal");
}
