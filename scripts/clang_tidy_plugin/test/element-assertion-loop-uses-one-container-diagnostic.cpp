// RUN: %check_clang_tidy -std=c++23 %s fastecu-container-assertion,fastecu-assertion-loop-trace %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gtest/gtest.h>
#include <vector>
TEST(Containers, Elements) {
    std::vector<int> values{1, 1};
    for (int value : values) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
        EXPECT_EQ(value, 1);
    }
    for (int& value : values) value = 2;
}
