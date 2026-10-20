// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-assertion %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
TEST(ResultAssertions, Callback) {
    auto verify = [] {
        auto result = ReadValue();
        ASSERT_THAT(result, fastecu::testing::IsErr(fastecu::ErrorKind::kTimeout));
        EXPECT_EQ(result.error().kind, fastecu::ErrorKind::kTimeout);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    };
    verify();
}
