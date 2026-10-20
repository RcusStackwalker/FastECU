// RUN: %check_clang_tidy -std=c++23 %s fastecu-uniform-bytes,fastecu-container-assertion %t_test -- \
// RUN:   %load %check_config -- %cxx_flags
// RUN: diff %t_test.orig %t_test.cpp

#include <gmock/gmock.h>
#include <algorithm>
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/testing/byte_matchers.h"
TEST(Bytes, Uniform) {
    bytes::Bytes data{0xff, 0xff};
    EXPECT_THAT(data, ::testing::Each(::testing::Eq(0xff)));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-uniform-bytes]
    EXPECT_EQ(data, bytes::Bytes(2, 0xff));
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-uniform-bytes]
    EXPECT_TRUE(
        std::all_of(data.begin(), data.end(),
// CHECK-MESSAGES: :[[@LINE-1]]:{{[0-9]+}}: warning: {{.*}} [fastecu-uniform-bytes]
                    [](bytes::Byte byte) { return byte == 0xff; }));
    EXPECT_THAT(data, test_bytes::BytesFilledWith(0xff));
    EXPECT_EQ(data, (bytes::Bytes{0xff, 0x00}));
    std::vector<int> numbers{1, 1};
    EXPECT_THAT(numbers, ::testing::Each(::testing::Eq(1)));
}
