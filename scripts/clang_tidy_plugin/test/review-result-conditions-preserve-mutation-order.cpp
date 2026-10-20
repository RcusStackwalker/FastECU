// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
fastecu::Result<int> ReadValue();
bool Mutate(fastecu::Result<int>&);
TEST(ResultAccess, Conditions) {
    auto first = ReadValue();
    if (first.has_value() && Mutate(first))
        EXPECT_EQ(*first, 1);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    auto second = ReadValue();
    if (second.has_value() || Mutate(second)) {}
    else EXPECT_EQ(second.error().kind, fastecu::ErrorKind::kTimeout);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    auto guarded = ReadValue();
    if (Mutate(guarded) || !guarded.has_value()) return;
    EXPECT_EQ(*guarded, 1);
}
