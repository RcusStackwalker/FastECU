// RUN: mkdir -p %t.dir/src/backend/ports
// RUN: %check_clang_tidy -std=c++23 %s fastecu-result-assertion %t.dir/src/backend/ports/result_test -- \
// RUN:   %load -- %cxx_flags
// RUN: diff %t.dir/src/backend/ports/result_test.orig %t.dir/src/backend/ports/result_test.cpp

#include <gtest/gtest.h>
#include "src/backend/ports/result.h"
TEST(InlineChecks, LowLevelResultApi) {
    fastecu::Result<int> result{42};
    ASSERT_TRUE(result.has_value());
}
