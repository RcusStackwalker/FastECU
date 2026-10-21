// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
int MutateAndCount(fastecu::Result<int>&);
void Consume(int, int);
TEST(ResultFlow, SiblingMutation) {
    auto result = ReadValue();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    Consume(*result, MutateAndCount(result));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    ASSERT_THAT(result, fastecu::testing::IsOk());
    const auto sum = *result + MutateAndCount(result);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    EXPECT_EQ(sum, 1);
}
