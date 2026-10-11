#include "src/backend/ports/testing/error_printers.h"

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
} // namespace
} // namespace fastecu
