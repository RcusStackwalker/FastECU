// RUN: %check_clang_tidy -std=c++23 %s fastecu-assertion-loop-trace %t_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gtest/gtest.h>
TEST(Loops, Trace) {
    SCOPED_TRACE("outside");
    for (int index = 0; index < 2; ++index) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-assertion-loop-trace]
        EXPECT_GE(index, 0);
    }
    for (int index = 0; index < 2; ++index) {
        SCOPED_TRACE(index);
        EXPECT_GE(index, 0);
    }
    for (int index = 0; index < 2; ++index) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-assertion-loop-trace]
        auto verify = [&] { EXPECT_GE(index, 0); };
        verify();
    }
    for (int outer = 0; outer < 2; ++outer) {
        SCOPED_TRACE(outer);
        for (int inner = 0; inner < 2; ++inner) {
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-assertion-loop-trace]
            EXPECT_GE(inner, 0);
        }
    }
    for (int index = 0; index < 2; ++index) {
        auto uncalled = [&] { EXPECT_GE(index, 0); };
        (void)uncalled;
    }
}
