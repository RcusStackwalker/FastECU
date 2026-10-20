// RUN: %check_clang_tidy -std=c++23 %s fastecu-assertion-loop-trace %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gtest/gtest.h>
TEST(LoopTrace, TraceOnly) {
    for (int outer = 0; outer < 2; ++outer)
        for (int inner = 0; inner < 2; ++inner) {
            SCOPED_TRACE(inner);
        }
}
TEST(LoopTrace, NestedAssertion) {
    for (int outer = 0; outer < 2; ++outer)
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-assertion-loop-trace]
        for (int inner = 0; inner < 2; ++inner) {
            SCOPED_TRACE(inner);
            EXPECT_EQ(inner, inner);
        }
}
