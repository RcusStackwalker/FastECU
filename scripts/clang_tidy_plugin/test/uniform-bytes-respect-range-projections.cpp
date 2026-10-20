// RUN: %check_clang_tidy -std=c++23 %s fastecu-uniform-bytes %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include <algorithm>
#include "src/algorithms/protocol/bytes.h"
TEST(Bytes, Projection) {
    bytes::Bytes data{1, 3};
    EXPECT_TRUE(
        std::ranges::all_of(data,
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-uniform-bytes]
            [](bytes::Byte byte) { return byte == 1; }));
    EXPECT_TRUE(std::ranges::all_of(data,
        [](bytes::Byte byte) { return byte == 1; },
        [](bytes::Byte byte) { return byte & 1; }));
}
