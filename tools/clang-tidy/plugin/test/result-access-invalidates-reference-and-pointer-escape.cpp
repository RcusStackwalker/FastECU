// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
void Mutate(fastecu::Result<int>&);
void MutatePointer(fastecu::Result<int>*);
TEST(ResultFlow, Aliases) {
    auto result = ReadValue();
    auto& alias = result;
    ASSERT_THAT(result, fastecu::testing::IsOk());
    EXPECT_EQ(*alias, 1);
    Mutate(alias);
    EXPECT_EQ(*result, 1);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    ASSERT_THAT(result, fastecu::testing::IsOk());
    MutatePointer(&result);
    EXPECT_EQ(*result, 1);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
}
