// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
TEST(ResultFlow, KnownBranch) {
    auto result = ReadValue();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    if (result.has_value()) EXPECT_EQ(*result, 1);
    EXPECT_EQ(*result, 1);
    auto other = ReadValue();
    do { EXPECT_EQ(*other, 1); } while (other.has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
}
TEST(ResultFlow, NegativeConjunction) {
    auto result = ReadValue();
    ASSERT_THAT(result, ::testing::Not(fastecu::testing::IsOkAnd(::testing::Eq(1))));
    EXPECT_EQ(result.error().kind, fastecu::ErrorKind::kTimeout);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
}
