#include "src/backend/ports/testing/error_printers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fastecu
{
namespace
{
TEST(ErrorPrinters, PrintsTheKindByName)
{
    EXPECT_EQ(::testing::PrintToString(ErrorKind::Timeout), "Timeout");
    EXPECT_EQ(::testing::PrintToString(ErrorKind::InvalidConfig), "InvalidConfig");
}

TEST(ErrorPrinters, PrintsAnErrorAsKindAndDetail)
{
    const Error error{ErrorKind::BadResponse, "unexpected NRC 0x78"};

    EXPECT_EQ(::testing::PrintToString(error), "BadResponse (unexpected NRC 0x78)");
}

TEST(ErrorPrinters, OmitsTheParenthesesWhenThereIsNoDetail)
{
    const Error error{ErrorKind::Cancelled, ""};

    EXPECT_EQ(::testing::PrintToString(error), "Cancelled");
}
} // namespace
} // namespace fastecu
