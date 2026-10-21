// RUN: %check_clang_tidy -std=c++23 %s fastecu-assertion-loop-trace,fastecu-result-access %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
TEST(ResultAccess, Lambdas) {
    [] {
        auto result = ReadValue();
        EXPECT_EQ(*result, 1);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    }();
    [] {
        auto result = ReadValue();
        ASSERT_THAT(result, fastecu::testing::IsOk());
        EXPECT_EQ(*result, 1);
    }();
    [] {
        for (int index = 0; index < 2; ++index)
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-assertion-loop-trace]
            EXPECT_EQ(index, index);
    }();
    auto captured = ReadValue();
    ASSERT_THAT(captured, fastecu::testing::IsOk());
    [&] { EXPECT_EQ(*captured, 1); }();
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    [&] {
        if (!captured.has_value()) return;
        EXPECT_EQ(*captured, 1);
    }();
}
