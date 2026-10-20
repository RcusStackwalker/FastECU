// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include <utility>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
void Mutate(fastecu::Result<int>&);
void MutateMoved(fastecu::Result<int>&&);
TEST(ResultAccess, Escape) {
    auto result = ReadValue();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    void (*mutate)(fastecu::Result<int>&) = &Mutate;
    mutate(result);
    EXPECT_EQ(*result, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    auto moved = ReadValue();
    ASSERT_THAT(moved, fastecu::testing::IsOk());
    MutateMoved(std::move(moved));
    EXPECT_EQ(*moved, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
}
