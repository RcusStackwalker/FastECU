// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
int Choice();
TEST(ResultAccess, Switch) {
    auto result = ReadValue();
    switch (Choice()) {
    case 0:
        ASSERT_THAT(result, fastecu::testing::IsOk());
        break;
    default: break;
    }
    EXPECT_EQ(*result, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(*result, 7);
}
