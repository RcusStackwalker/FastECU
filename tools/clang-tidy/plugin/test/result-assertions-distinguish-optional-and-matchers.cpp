// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-assertion %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include <expected>
#include <optional>
#include <string>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
bool CheckFlags(bool flags) { return flags; }
struct Flag { bool value; explicit operator bool() const { return value; } };
TEST(InlineChecks, ResultAssertions) {
    fastecu::Result<int> result{42};
    fastecu::Status status{};
    std::optional<int> optional{42};
    fastecu::Result<std::optional<int>> nested{std::optional<int>{42}};
    const auto *pointer = &result;
    EXPECT_TRUE(result.has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    ASSERT_TRUE((status.has_value()));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    EXPECT_FALSE(!pointer->has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    ASSERT_FALSE(result.has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    EXPECT_TRUE(nested.has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    EXPECT_TRUE(optional.has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    EXPECT_FALSE(!optional.has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    EXPECT_TRUE(nested->has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    std::expected<int, std::string> other{42};
    EXPECT_TRUE(other.has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    ASSERT_FALSE(other.has_value());
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    EXPECT_THAT(optional, ::testing::Optional(42));
    EXPECT_THAT(optional, ::testing::Eq(std::nullopt));
    EXPECT_THAT(other, fastecu::testing::IsOk());
    EXPECT_THAT(result, fastecu::testing::IsOk());
    EXPECT_TRUE(result.has_value() || optional.has_value());
    EXPECT_TRUE(CheckFlags(result.has_value()));
    EXPECT_TRUE(Flag{result.has_value()});
    auto consume = [](auto action) { action(); };
    consume([&] { EXPECT_TRUE(result.has_value()); });
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-assertion]
    if (result.has_value()) { EXPECT_EQ(*result, 42); }
}
