#include "src/backend/ports/testing/error_printers.h"

#include <expected>

#include "src/backend/ports/result.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu
{
namespace
{
TEST(ErrorPrinters, PrintsTheKindByName)
{
    EXPECT_EQ(::testing::PrintToString(ErrorKind::kTimeout), "Timeout");
    EXPECT_EQ(::testing::PrintToString(ErrorKind::kInvalidConfig), "InvalidConfig");
}

TEST(ErrorPrinters, PrintsAnErrorAsKindAndDetail)
{
    const Error error{ErrorKind::kBadResponse, "unexpected NRC 0x78"};

    EXPECT_EQ(::testing::PrintToString(error), "BadResponse (unexpected NRC 0x78)");
}

TEST(ErrorPrinters, OmitsTheParenthesesWhenThereIsNoDetail)
{
    const Error error{ErrorKind::kCancelled, ""};

    EXPECT_EQ(::testing::PrintToString(error), "Cancelled");
}

TEST(ErrorPrinters, PrintsAResultValue)
{
    EXPECT_EQ(::testing::PrintToString(Result<int>{42}), "ok: 42");
}

TEST(ErrorPrinters, PrintsAResultError)
{
    const Result<int> result = std::unexpected(Error{ErrorKind::kTimeout, "no reply"});

    EXPECT_EQ(::testing::PrintToString(result), "error: Timeout (no reply)");
}

TEST(ErrorPrinters, PrintsAStatus)
{
    EXPECT_EQ(::testing::PrintToString(Status{}), "ok");
    EXPECT_EQ(::testing::PrintToString(Status{std::unexpected(Error{ErrorKind::kCancelled, ""})}), "error: Cancelled");
}

// A value type with no printer still compiles; only the value falls back to bytes.
TEST(ErrorPrinters, PrintsAResultOfAnUnprintableValue)
{
    struct NoPrinter
    {
        int x;
    };

    EXPECT_TRUE(::testing::PrintToString(Result<NoPrinter>{NoPrinter{1}}).starts_with("ok: "));
}
} // namespace
} // namespace fastecu
