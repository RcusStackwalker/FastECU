// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
using fastecu::testing::IsOk;
fastecu::Result<int> ReadValue();
void Mutate(fastecu::Result<int>&);
void Observe(const fastecu::Result<int>&);
bool Coin();
TEST(ResultAccess, Mutation) {
    auto changed = ReadValue();
    ASSERT_THAT(changed, IsOk());
    changed = fastecu::Fail(fastecu::ErrorKind::kTimeout, "timeout");
    EXPECT_EQ(*changed, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    auto passed = ReadValue();
    ASSERT_THAT(passed, IsOk());
    Observe(passed);
    EXPECT_EQ(*passed, 7);
    Mutate(passed);
    EXPECT_EQ(*passed, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    auto joined = ReadValue();
    ASSERT_THAT(joined, IsOk());
    if (Coin()) joined = ReadValue();
    EXPECT_EQ(*joined, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    auto stable = ReadValue();
    ASSERT_THAT(stable, IsOk());
    for (int index = 0; index < 2; ++index) EXPECT_EQ(*stable, index);
    for (int index = 0; index < 2; ++index) {
        auto fresh = ReadValue();
        ASSERT_THAT(fresh, IsOk());
        EXPECT_EQ(*fresh, index);
    }
}
