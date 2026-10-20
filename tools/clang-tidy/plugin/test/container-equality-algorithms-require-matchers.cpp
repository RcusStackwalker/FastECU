// RUN: %check_clang_tidy -std=c++23 %s fastecu-container-assertion %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gtest/gtest.h>
#include <algorithm>
#include <vector>
TEST(Containers, Equality) {
    std::vector<int> actual{1, 2};
    std::vector<int> expected{1, 2};
    EXPECT_TRUE(
        std::equal(actual.begin(), actual.end(),
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
                   expected.begin(), expected.end()));
    EXPECT_TRUE(std::ranges::equal(actual, expected));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
    EXPECT_EQ(actual, expected);
}
