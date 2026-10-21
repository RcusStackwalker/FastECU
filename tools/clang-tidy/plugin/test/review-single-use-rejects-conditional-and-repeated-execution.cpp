// RUN: %check_clang_tidy -std=c++23 %s fastecu-single-use-result %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Status Act();
bool Enabled();
TEST(SingleUse, Conditional) {
    auto status = Act();
    if (Enabled()) EXPECT_THAT(status, fastecu::testing::IsOk());
}
TEST(SingleUse, Repeated) {
    auto status = Act();
    for (int index = 0; index < 2; ++index)
        EXPECT_THAT(status, fastecu::testing::IsOk());
}
