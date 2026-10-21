// RUN: %check_clang_tidy -std=c++23 %s fastecu-container-assertion %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <algorithm>
#include <gmock/gmock.h>
#include <vector>
bool CheckFlags(bool flags) { return flags; }
TEST(InlineChecks, ContainerAssertions) {
    std::vector<int> values{1, 2};
    auto positive = [](int value) { return value > 0; };
    EXPECT_TRUE(std::ranges::all_of(values, positive));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
    ASSERT_FALSE(std::ranges::any_of(values, positive));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
    EXPECT_FALSE(std::ranges::none_of(values, positive));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
    ASSERT_TRUE(std::all_of(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
        values.begin(), values.end(), positive));
    EXPECT_FALSE(std::any_of(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
        values.begin(), values.end(), positive));
    ASSERT_FALSE(std::none_of(
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
        values.begin(), values.end(), positive));
    EXPECT_THAT(values, ::testing::Each(::testing::Gt(0)));
    const bool all_positive = std::ranges::all_of(values, positive);
    EXPECT_TRUE(all_positive);
    EXPECT_TRUE(std::ranges::all_of(values, positive) || values.empty());
    EXPECT_TRUE(CheckFlags(std::ranges::all_of(values, positive)));
    auto consume = [](auto action) { action(); };
    consume([&] {
        EXPECT_TRUE(std::ranges::all_of(values, positive));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-container-assertion]
    });
}
