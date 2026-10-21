// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
void Consume(int);
bool Again(int);
TEST(ResultAccess, ContinueIncrement) {
    auto result = ReadValue();
    for (int index = 0; index < 2; Consume(*result), ++index) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
        if (index == 0) continue;
        ASSERT_THAT(result, fastecu::testing::IsOk());
    }
}
TEST(ResultAccess, ContinueCondition) {
    auto result = ReadValue();
    int index = 0;
    do {
        if (index++ == 0) continue;
        ASSERT_THAT(result, fastecu::testing::IsOk());
    } while (Again(*result));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
}
TEST(ResultAccess, ReadonlyContinue) {
    auto result = ReadValue();
    ASSERT_THAT(result, fastecu::testing::IsOk());
    for (int index = 0; index < 2; Consume(*result), ++index) {
        if (index == 0) continue;
    }
}
