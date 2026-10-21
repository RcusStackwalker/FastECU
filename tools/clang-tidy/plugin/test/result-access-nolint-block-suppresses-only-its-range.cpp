// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
fastecu::Result<int> ReadValue();
class SharedSetup : public ::testing::Test {
protected:
    void SetUp() override { ASSERT_THAT(result, fastecu::testing::IsOk()); }
    const fastecu::Result<int> result = ReadValue();
};
// NOLINTBEGIN(fastecu-result-access): SetUp proves success; the body is skipped after a fatal SetUp failure.
TEST_F(SharedSetup, Allowed) { EXPECT_EQ(*result, 1); }
// NOLINTEND(fastecu-result-access)
TEST_F(SharedSetup, Other) { EXPECT_EQ(*result, 1); }
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
