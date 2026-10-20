// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-assertion %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include <optional>
fastecu::Result<int> ReadValue();
TEST(ResultAssertions, Conversion) {
    auto result = ReadValue();
    ASSERT_TRUE(result);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    EXPECT_FALSE(!result);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    std::optional<int> optional{1};
    ASSERT_TRUE(optional);
}
