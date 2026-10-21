// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-access %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include "src/backend/ports/result.h"
#include "src/backend/ports/testing/result_matchers.h"
using fastecu::testing::IsOk;
using fastecu::testing::IsErr;
fastecu::Result<int> ReadValue();
TEST(ResultAccess, Guards) {
    auto unsafe = ReadValue();
    EXPECT_THAT(unsafe, IsOk());
    EXPECT_EQ(*unsafe, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    auto good = ReadValue();
    ASSERT_THAT(good, IsOk());
    EXPECT_EQ(*good, 7);
    auto error = ReadValue();
    ASSERT_THAT(error, IsErr(fastecu::ErrorKind::kTimeout));
    EXPECT_EQ(error.error().kind, fastecu::ErrorKind::kTimeout);
    EXPECT_EQ(*error, 7);
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-result-access]
    auto guarded = ReadValue();
    if (!guarded.has_value()) return;
    EXPECT_EQ(*guarded, 7);
    if (guarded.has_value()) EXPECT_EQ(*guarded, 7);
    else EXPECT_EQ(guarded.error().kind, fastecu::ErrorKind::kTimeout);
}
