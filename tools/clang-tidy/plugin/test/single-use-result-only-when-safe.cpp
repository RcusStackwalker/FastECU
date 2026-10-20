// RUN: %check_clang_tidy -std=c++23 %s fastecu-single-use-result %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include <memory>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Status Act();
fastecu::Result<std::unique_ptr<int>> Own();
void Observe();
TEST(SingleUse, Safe) {
    auto ordered = Act();
    Observe();
    ASSERT_THAT(ordered, fastecu::testing::IsOk());
    auto owned = Own();
    ASSERT_THAT(owned, fastecu::testing::IsOk());
    auto reused = Act();
    ASSERT_THAT(reused, fastecu::testing::IsOk());
    EXPECT_THAT(reused, fastecu::testing::IsOk());
    auto status = Act();
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-single-use-result]
    ASSERT_THAT(status, fastecu::testing::IsOk());
}
