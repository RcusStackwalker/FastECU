// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-assertion %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
// NOLINTBEGIN(fastecu-result-assertion): exercises the negation of IsOk itself.
TEST(Nolint, Allowed) {
    EXPECT_THAT(ReadValue(), ::testing::Not(fastecu::testing::IsOk()));
}
// NOLINTEND(fastecu-result-assertion)
TEST(Nolint, Other) {
    EXPECT_THAT(ReadValue(), ::testing::Not(fastecu::testing::IsOk()));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
}
