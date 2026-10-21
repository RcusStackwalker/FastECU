// RUN: %check_clang_tidy -std=c++23 %s fastecu-parameterized-case-name %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gtest/gtest.h>
class Cases : public ::testing::TestWithParam<int> {};
TEST_P(Cases, Value) { EXPECT_GE(GetParam(), 0); }
INSTANTIATE_TEST_SUITE_P(Unnamed, Cases, ::testing::Values(0, 1));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-parameterized-case-name]
INSTANTIATE_TEST_SUITE_P(Named, Cases, ::testing::Values(0, 1),
                         ::testing::PrintToStringParamName());
