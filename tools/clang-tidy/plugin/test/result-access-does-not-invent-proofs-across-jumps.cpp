// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
bool Coin();
TEST(ResultAccess, Jump) {
    auto result = ReadValue();
    if (Coin()) goto access;
    ASSERT_THAT(result, fastecu::testing::IsOk());
access:
    EXPECT_EQ(*result, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
}
TEST(ResultAccess, Exception) {
    auto result = ReadValue();
    try {
        if (Coin()) throw 1;
        ASSERT_THAT(result, fastecu::testing::IsOk());
    } catch (...) {}
    EXPECT_EQ(*result, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
}
